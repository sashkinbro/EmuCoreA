#!/usr/bin/env python3
"""Extract the XMB's images, sounds, models and fonts from a local flash0 dump.

Output is the ready resource tree that the pack builder zips and the application
unpacks on first start. Firmware and resources stay out of Git.

The resource selection is deliberate: these are the containers that make up the
XMB's shell and its Game column. Every extracted file records its source RCO,
entry label and the SHA-256 of both the payload and the written file, so a
difference can be traced back to one container entry.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import sys
import time
import zlib
from dataclasses import asdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "native-ui" / "tools"))

from psp import gim, rco, vag  # noqa: E402

DEFAULT_FLASH0 = REPO / "native-ui" / "firmware" / "flash0"
DEFAULT_OUT = REPO / "native-ui" / "resources"

# Containers that belong to the shell and the Game column.
RCO_FILES = (
    "system_plugin.rco",
    "system_plugin_bg.rco",
    "system_plugin_fg.rco",
    "topmenu_plugin.rco",
    "topmenu_icon.rco",
    "game_plugin.rco",
    "savedata_plugin.rco",
    "opening_plugin.rco",
    "impose_plugin.rco",
)

FONT_FILES = ("ltn0.pgf", "ltn1.pgf", "ltn2.pgf", "ltn3.pgf", "ltn4.pgf",
              "ltn5.pgf", "ltn6.pgf", "ltn7.pgf", "ltn8.pgf", "ltn9.pgf",
              "ltn10.pgf", "ltn11.pgf", "ltn12.pgf", "ltn13.pgf", "ltn14.pgf",
              "ltn15.pgf")

SAFE = "-_.()"


def safe_name(label: str, fallback: str) -> str:
    cleaned = "".join(c if c.isalnum() or c in SAFE else "_" for c in label).strip("._")
    return cleaned or fallback


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_firmware_version(flash0: Path) -> str:
    for candidate in (flash0 / "vsh" / "etc" / "version.txt", flash0 / "vsh" / "etc" / "version.txt"):
        if candidate.is_file():
            text = candidate.read_text(encoding="utf-8", errors="replace")
            for line in text.splitlines():
                if ":" in line:
                    key, value = line.split(":", 1)
                    if key.strip() == "release":
                        return value.strip()
                elif line.strip():
                    return line.strip()
    return "unknown"


def extract_into(flash0: Path, staging: Path) -> dict:
    resource_dir = flash0 / "vsh" / "resource"
    if not resource_dir.is_dir():
        raise SystemExit(f"no vsh/resource under {flash0}")
    (staging / "images").mkdir(parents=True)
    (staging / "audio").mkdir()
    (staging / "fonts").mkdir()
    (staging / "models").mkdir()
    (staging / "rco").mkdir()

    files: list[dict] = []
    counts = {"images": 0, "audio": 0, "models": 0, "fonts": 0, "raw": 0, "rco": 0}

    for name in RCO_FILES:
        path = resource_dir / name
        if not path.is_file():
            print(f"skip missing {name}")
            continue
        document = rco.open_rco(path)
        stem = path.stem
        used: dict[str, int] = {}

        def target(kind: str, label: str, offset: int, suffix: str) -> Path:
            base = safe_name(label, f"{kind}_{offset:06x}")
            used[(kind, base)] = used.get((kind, base), 0) + 1
            if used[(kind, base)] > 1:
                base = f"{base}_{used[(kind, base)]}"
            return staging / kind / stem / (base + suffix)

        for image in document.images():
            destination = target("images", image.label, image.offset, ".png")
            destination.parent.mkdir(parents=True, exist_ok=True)
            record = {
                "kind": "image", "rco": name, "label": image.label,
                "format": image.format, "compression": image.compression,
                "packed_size": image.packed_size, "unpacked_size": image.unpacked_size,
                "sha256_source": sha256(image.data),
            }
            try:
                decoded = gim.decode(image.data)
                payload = gim.png_bytes(decoded)
                destination.write_bytes(payload)
                record.update(width=decoded.width, height=decoded.height,
                              file=str(destination.relative_to(staging)),
                              sha256_file=sha256(payload))
                counts["images"] += 1
            except gim.GimError as error:
                # Keep the original payload so no image is silently lost; the
                # format is then handled before it enters the pack.
                destination = destination.with_suffix(".gim")
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(image.data)
                record.update(error=str(error), file=str(destination.relative_to(staging)),
                              sha256_file=sha256(image.data))
                counts["raw"] += 1
            files.append(record)

        for sound in document.sounds():
            decoded = []
            for channel in sound.channels:
                decoded.append(vag.decode(channel.data))
            if not decoded:
                continue
            if len(decoded) == 1:
                wav = vag.wav(decoded[0], 1)
                rate = decoded[0].sample_rate
                frames = len(decoded[0].samples) // 2
            else:
                left, right = decoded[0], decoded[1]
                frames = min(len(left.samples), len(right.samples)) // 2
                samples = bytearray()
                for i in range(frames):
                    samples += left.samples[i * 2:i * 2 + 2]
                    samples += right.samples[i * 2:i * 2 + 2]
                merged = vag.Vag(left.sample_rate, bytes(samples), None, None, left.name)
                wav = vag.wav(merged, 2)
                rate = left.sample_rate
            destination = target("audio", sound.label, sound.offset, ".wav")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(wav)
            files.append({
                "kind": "audio", "rco": name, "label": sound.label,
                "format": sound.format, "channels": len(decoded), "sample_rate": rate,
                "frames": frames, "file": str(destination.relative_to(staging)),
                "sha256_file": sha256(wav),
                "sha256_source": [sha256(c.data) for c in sound.channels],
            })
            counts["audio"] += 1

        for model in document.models():
            destination = target("models", model.label, model.offset, ".gmo")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(model.data)
            files.append({
                "kind": "model", "rco": name, "label": model.label,
                "format": model.format, "compression": model.compression,
                "packed_size": model.packed_size, "unpacked_size": model.unpacked_size,
                "file": str(destination.relative_to(staging)),
                "sha256_file": sha256(model.data), "sha256_source": sha256(model.data),
            })
            counts["models"] += 1

        print(f"{name}: {len(document.images())} images, {len(document.sounds())} sounds, "
              f"{len(document.models())} models, {len(document.tables)} tables")

    font_dir = flash0 / "font"
    for font in FONT_FILES:
        source = font_dir / font
        if not source.is_file():
            continue
        destination = staging / "fonts" / font
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(source.read_bytes())
        files.append({
            "kind": "font", "file": str(destination.relative_to(staging)),
            "sha256_file": sha256(destination.read_bytes()),
            "sha256_source": sha256(source.read_bytes()),
        })
        counts["fonts"] += 1

    # English UI strings straight from the shell's text tables. The renderer
    # uses these keys instead of embedding labels of its own.
    (staging / "text").mkdir()
    strings: dict[str, str] = {}
    for name in ("topmenu_plugin.rco", "game_plugin.rco"):
        path = resource_dir / name
        if not path.is_file():
            continue
        document = rco.open_rco(path)
        for entry in document.iter_entries(3):
            if entry.type != 1 or len(entry.extra) < 8:
                continue
            lang = struct.unpack_from("<H", entry.extra, 0)[0]
            if lang != 1:
                continue
            strings.update(document.text_entry_strings(entry))
    if strings:
        lines = [f"{key}\t{value.replace(chr(10), '\\n')}" for key, value in sorted(strings.items())]
        destination = staging / "text" / "en.txt"
        destination.write_text("\n".join(lines) + "\n", encoding="utf-8")
        files.append({
            "kind": "text", "language": "en", "count": len(strings),
            "file": str(destination.relative_to(staging)),
            "sha256_file": sha256(destination.read_bytes()),
        })
        counts["text"] = len(strings)

    # The wave needs the RCO's ModelObject transform (position/scale/tint), not
    # just the GMO vertices. Keep this small container so the runtime can load
    # the exact transform instead of guessing it.
    background = resource_dir / "system_plugin_bg.rco"
    if background.is_file():
        destination = staging / "rco" / background.name
        destination.write_bytes(background.read_bytes())
        files.append({
            "kind": "rco", "file": str(destination.relative_to(staging)),
            "sha256_file": sha256(destination.read_bytes()),
            "sha256_source": sha256(background.read_bytes()),
        })
        counts["rco"] += 1

    extract_theme_assets(resource_dir, staging, files, counts)

    manifest = {
        "schema": 1,
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "firmware": read_firmware_version(flash0),
        "counts": counts,
        "files": sorted(files, key=lambda entry: entry["file"]),
    }
    (staging / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def bmp24_to_png(data: bytes) -> tuple[int, int, bytes]:
    """Decode an uncompressed 24bpp Windows BMP into PNG bytes without external libs."""
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("not a BMP")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width, raw_height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if width <= 0 or raw_height == 0 or bpp != 24:
        raise ValueError(f"unsupported BMP {width}x{raw_height} {bpp}bpp")
    top_down = raw_height < 0
    height = abs(raw_height)
    row_stride = ((width * 3) + 3) & ~3
    rgba = bytearray(width * height * 4)
    for y in range(height):
        src_y = y if top_down else (height - 1 - y)
        row_start = pixel_offset + src_y * row_stride
        dst_start = y * width * 4
        for x in range(width):
            b, g, r = data[row_start + x * 3 : row_start + x * 3 + 3]
            rgba[dst_start + x * 4 : dst_start + x * 4 + 4] = bytes((r, g, b, 255))
    image = gim.Image(width=width, height=height, format=3, pixel_order=0, bpp=32, pixels=bytes(rgba))
    return width, height, gim.png_bytes(image)


def extract_theme_assets(resource_dir: Path, staging: Path, files: list[dict], counts: dict) -> None:
    """Extract PSP 6.60 theme palettes (01-12/13-27.bmp) and built-in Cookies PTF."""
    themes_dir = staging / "images" / "themes"
    themes_dir.mkdir(parents=True, exist_ok=True)

    def write_theme_png(name: str, w: int, h: int, payload: bytes, src_digest: str) -> None:
        dest = themes_dir / f"{name}.png"
        dest.write_bytes(payload)
        files.append({
            "kind": "image",
            "rco": "theme",
            "label": name,
            "width": w,
            "height": h,
            "file": str(dest.relative_to(staging)),
            "sha256_file": sha256(payload),
            "sha256_source": src_digest,
        })
        counts["images"] += 1

    def extract_bmp_strip(src_name: str, prefix: str, expected_count: int) -> None:
        src = resource_dir / src_name
        if not src.is_file():
            return
        raw = src.read_bytes()
        chunk_size = 6176  # 60x34 24bpp BMP (6174 + 2-byte alignment)
        count = len(raw) // chunk_size
        for i in range(min(count, expected_count)):
            chunk = raw[i * chunk_size : (i + 1) * chunk_size]
            if chunk[:2] != b"BM":
                continue
            w, h, png = bmp24_to_png(chunk)
            write_theme_png(f"{prefix}_{i:02d}", w, h, png, sha256(chunk))

    orig_name = "01-12_03g.bmp" if (resource_dir / "01-12_03g.bmp").is_file() else "01-12.bmp"
    extract_bmp_strip(orig_name, "orig", 12)
    extract_bmp_strip("01-12.bmp", "classic", 12)
    extract_bmp_strip("13-27.bmp", "ext", 22)

    ptf_path = resource_dir / "custom_theme.dat"
    if ptf_path.is_file():
        ptf = ptf_path.read_bytes()
        if len(ptf) >= 0x114 and ptf[:4] == b"\x00PTF":
            offs = struct.unpack_from("<5I", ptf, 0x100)
            # Section 1: 480x272 wallpaper BMP
            sec1 = ptf[offs[1] : offs[2]]
            zp = sec1.find(b"\x78\x9c")
            if zp >= 0:
                bmp = zlib.decompress(sec1[zp:])
                w, h, png = bmp24_to_png(bmp)
                write_theme_png("cookies_bg", w, h, png, sha256(bmp))
            # Section 2: 8 category GIMs -> map PTF order to XMB 0..7
            sec2 = ptf[offs[2] : offs[3]]
            gims: list[bytes] = []
            pos = 0
            while len(gims) < 8:
                zp = sec2.find(b"\x78\x9c", pos)
                if zp < 0:
                    break
                try:
                    dec = zlib.decompress(sec2[zp:])
                    if dec.startswith(b"MIG."):
                        gims.append(dec)
                except zlib.error:
                    pass
                pos = zp + 2
            ptf_to_xmb = [1, 7, 2, 3, 4, 5, 6, 0]
            if len(gims) == 8:
                for xmb_cat, ptf_idx in enumerate(ptf_to_xmb):
                    decoded = gim.decode(gims[ptf_idx])
                    write_theme_png(
                        f"cookies_cat_{xmb_cat}",
                        decoded.width,
                        decoded.height,
                        gim.png_bytes(decoded),
                        sha256(gims[ptf_idx]),
                    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--flash0", type=Path, default=DEFAULT_FLASH0)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--force", action="store_true", help="replace an existing resource tree")
    args = parser.parse_args(argv)

    if args.out.exists() and not args.force:
        print(f"error: {args.out} already exists (use --force)", file=sys.stderr)
        return 1
    staging = args.out.parent / (args.out.name + ".staging")
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    manifest = extract_into(args.flash0, staging)
    if args.out.exists():
        shutil.rmtree(args.out)
    staging.rename(args.out)
    print(f"wrote {args.out}: "
          f"{manifest['counts']['images']} images, {manifest['counts']['audio']} sounds, "
          f"{manifest['counts']['models']} models, {manifest['counts']['fonts']} fonts, "
          f"{manifest['counts']['rco']} rco, {manifest['counts']['raw']} raw fallbacks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

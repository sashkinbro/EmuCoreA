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

    manifest = {
        "schema": 1,
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "firmware": read_firmware_version(flash0),
        "counts": counts,
        "files": sorted(files, key=lambda entry: entry["file"]),
    }
    (staging / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


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

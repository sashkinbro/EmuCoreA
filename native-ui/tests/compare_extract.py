#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SBRO
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare two extracted resource trees (e.g. the Python extractor's and the
native xmb_extract port's): same manifest entries, identical bytes for non-PNG
files, identical decoded RGBA for PNGs.

Manifest paths are normalised to forward slashes and text files to LF, because
the reference may have been produced on Windows.

    python compare_extract.py REFERENCE_DIR CANDIDATE_DIR
"""
import json
import struct
import sys
import zlib
from pathlib import Path


def load(root: Path):
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    for entry in manifest["files"]:
        entry["file"] = entry["file"].replace("\\", "/")
    return manifest


def png_rgba(data: bytes):
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, size = 8, b"", None
    while pos < len(data):
        n, tag = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            size = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    w, h, depth, ctype, _, _, interlace = size
    assert (depth, ctype, interlace) == (8, 6, 0)
    raw = zlib.decompress(idat)
    stride = w * 4
    assert len(raw) == (stride + 1) * h
    rows = bytearray()
    for y in range(h):
        assert raw[y * (stride + 1)] == 0, "only filter 0 supported"
        rows += raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)]
    return w, h, bytes(rows)


def main() -> int:
    ref, cand = Path(sys.argv[1]), Path(sys.argv[2])
    a, b = load(ref), load(cand)
    mismatches = 0

    def bad(msg):
        nonlocal mismatches
        mismatches += 1
        print("MISMATCH", msg)

    for key in ("schema", "firmware", "counts"):
        if a[key] != b[key]:
            bad(f"{key}: {a[key]} != {b[key]}")
    ea = {e["file"]: e for e in a["files"]}
    eb = {e["file"]: e for e in b["files"]}
    if len(ea) != len(a["files"]) or len(eb) != len(b["files"]):
        bad("duplicate manifest paths")
    for path in sorted(set(ea) ^ set(eb)):
        bad(f"path only on one side: {path}")
    png = same_bytes = 0
    for path in sorted(set(ea) & set(eb)):
        x, y = dict(ea[path]), dict(eb[path])
        is_png = path.endswith(".png")
        is_text = path.endswith(".txt")
        if is_png or is_text:
            x.pop("sha256_file")
            y.pop("sha256_file")
        if x != y:
            bad(f"manifest entry {path}: {x} != {y}")
        if list(x) != list(y):
            bad(f"field order {path}")
        fa, fb = (ref / path).read_bytes(), (cand / path).read_bytes()
        if is_png:
            png += 1
            if png_rgba(fa) != png_rgba(fb):
                bad(f"pixels differ: {path}")
        else:
            if is_text:
                fa = fa.replace(b"\r\n", b"\n")
            same_bytes += 1
            if fa != fb:
                bad(f"bytes differ: {path}")
    print(f"entries ref={len(ea)} cand={len(eb)} png_pixel_checked={png} byte_checked={same_bytes} mismatches={mismatches}")
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())

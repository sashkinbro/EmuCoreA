#!/usr/bin/env python3
"""Pack the ready resource tree into the single asset the application ships.

The ZIP is deterministic (sorted entries, fixed timestamps, stored directories
omitted) so the same resources always give the same SHA-256. The hash is the
pack version the Android installer uses to decide whether a first-run
installation is already current.

Only ready files may enter the pack: no firmware containers, no raw source
payloads and no paths outside the resource root.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_RESOURCES = REPO / "native-ui" / "resources"
DEFAULT_OUTPUT = REPO / "native-ui" / "build" / "native-ui.zip"
MAX_FILES = 4096
MAX_UNCOMPRESSED = 64 * 1024 * 1024
FIXED_TIME = (1980, 1, 1, 0, 0, 0)


class PackError(ValueError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect(resources: Path) -> list[Path]:
    if not (resources / "manifest.json").is_file():
        raise PackError(f"{resources} has no manifest.json")
    files: list[Path] = []
    total = 0
    for path in sorted(resources.rglob("*")):
        if path.is_symlink():
            raise PackError(f"symlink in resource tree: {path}")
        if not path.is_file():
            continue
        relative = path.relative_to(resources)
        if relative.is_absolute() or ".." in relative.parts:
            raise PackError(f"escaping resource path: {relative}")
        total += path.stat().st_size
        files.append(relative)
        if len(files) > MAX_FILES:
            raise PackError("resource tree has too many files")
        if total > MAX_UNCOMPRESSED:
            raise PackError("resource tree is over the unpack size limit")
    if not files:
        raise PackError("resource tree is empty")
    return files


def build(resources: Path, output: Path) -> dict:
    files = collect(resources)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".tmp")
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for relative in files:
            info = zipfile.ZipInfo(relative.as_posix(), FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, (resources / relative).read_bytes())
    temporary.replace(output)
    digest = sha256_file(output)
    pack = {
        "schema": 1,
        "sha256": digest,
        "size": output.stat().st_size,
        "files": len(files),
        "uncompressed_size": sum((resources / relative).stat().st_size for relative in files),
    }
    metadata = output.with_name("native-ui-pack.json")
    metadata.write_text(json.dumps(pack, indent=2) + "\n", encoding="utf-8")
    return pack


def verify(resources: Path, output: Path, pack: dict) -> None:
    if sha256_file(output) != pack["sha256"]:
        raise PackError("pack hash changed after writing")
    with zipfile.ZipFile(output) as archive:
        names = archive.namelist()
        if names != sorted(names):
            raise PackError("zip entries are not sorted")
        if len(names) != pack["files"]:
            raise PackError("zip entry count does not match the manifest")
        total = 0
        for name in names:
            if name.startswith("/") or ".." in Path(name).parts:
                raise PackError(f"escaping zip entry: {name}")
            data = archive.read(name)
            if data != (resources / name).read_bytes():
                raise PackError(f"zip entry differs from the resource tree: {name}")
            total += len(data)
        if total != pack["uncompressed_size"]:
            raise PackError("zip unpacked size does not match")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--resources", type=Path, default=DEFAULT_RESOURCES)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args(argv)
    try:
        pack = build(args.resources, args.output)
        verify(args.resources, args.output, pack)
    except PackError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"packed {pack['files']} files into {args.output}")
    print(f"  sha256 {pack['sha256']}")
    print(f"  {pack['size']} bytes packed, {pack['uncompressed_size']} bytes unpacked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

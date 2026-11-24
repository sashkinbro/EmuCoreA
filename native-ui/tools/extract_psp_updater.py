#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SBRO
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract a PSP firmware updater from a UMD disc image.

Most PSP discs carry the firmware updater in PSP_GAME/SYSDIR/UPDATE/. The file
is a bare PSAR archive, so it can be handed to a PSAR unpacker directly; nothing
here decrypts it. The updater's own PARAM.SFO names the firmware version, which
is printed before the file is written.

Both plain ISO9660 images (2048-byte sectors) and CISO-compressed images are
read. Only the ISO9660 directory structures needed to reach the updater are
implemented; this is not a general-purpose ISO tool.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

SECTOR_SIZE = 2048
UPDATE_DIR = ("PSP_GAME", "SYSDIR", "UPDATE")
DATA_BIN = "DATA.BIN"
PARAM_SFO = "PARAM.SFO"


class ImageError(RuntimeError):
    pass


@dataclass(frozen=True)
class Entry:
    name: str
    extent: int
    size: int
    is_dir: bool


class DiscImage:
    """Raw 2048-byte-sector view of a disc image."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self._file = path.open("rb")
        self._file.seek(0, 2)
        self.size = self._file.tell()

    def close(self) -> None:
        self._file.close()

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.size:
            raise ImageError(f"read outside image: offset={offset} size={size}")
        self._file.seek(offset)
        data = self._file.read(size)
        if len(data) != size:
            raise ImageError(f"short read at {offset}: {len(data)} != {size}")
        return data


class CisoImage(DiscImage):
    """CISO/CSO image. Compression is per 2048-byte block, optional zlib."""

    def __init__(self, path: Path) -> None:
        super().__init__(path)
        header = DiscImage.read(self, 0, 0x18)
        if header[:4] != b"CISO":
            raise ImageError(f"{path.name}: not a CISO image")
        self.header_size, self.uncompressed_size = struct.unpack_from("<IQ", header, 4)
        self.block_size, self.version, self.align = struct.unpack_from("<IBB", header, 0x10)
        if self.header_size < 0x18 or self.block_size != SECTOR_SIZE:
            raise ImageError(f"{path.name}: unexpected CISO header")
        self.block_count = (self.uncompressed_size + self.block_size - 1) // self.block_size
        table = DiscImage.read(self, self.header_size, self.block_count * 4)
        self.index = struct.unpack(f"<{self.block_count}I", table)
        self.data_offset = self.header_size + self.block_count * 4

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.uncompressed_size:
            raise ImageError(f"read outside image: offset={offset} size={size}")
        out = bytearray()
        while size > 0:
            block = offset // self.block_size
            within = offset % self.block_size
            take = min(size, self.block_size - within)
            out += self._block(block)[within : within + take]
            offset += take
            size -= take
        return bytes(out)

    def _block(self, index: int) -> bytes:
        if index >= self.block_count:
            raise ImageError("CISO block index outside image")
        entry = self.index[index]
        if entry & 0x80000000:
            return self.read_raw(self.data_offset + (entry & 0x7FFFFFFF) * self.block_size, self.block_size)
        start = self.data_offset + (entry << self.align)
        # A zlib block is never larger than its uncompressed size by much; read
        # a bounded window and let zlib ignore whatever follows the stream.
        length = min(self.size - start, self.block_size * 2)
        return zlib.decompress(self.read_raw(start, length))[: self.block_size]

    def read_raw(self, offset: int, size: int) -> bytes:
        # DiscImage.read() is bounds-checked against the logical image size;
        # compressed data is addressed against the physical file instead.
        self._file.seek(offset)
        data = self._file.read(size)
        if len(data) != size:
            raise ImageError(f"short read at {offset}: {len(data)} != {size}")
        return data


def _both_endian_u32(data: bytes, offset: int) -> int:
    le = struct.unpack_from("<I", data, offset)[0]
    be = struct.unpack_from(">I", data, offset + 4)[0]
    if le != be:
        raise ImageError(f"both-endian field disagrees at {offset}: {le} != {be}")
    return le


class Iso9660:
    """Minimal ISO9660 reader: PVD, directory records, file extents."""

    def __init__(self, image: DiscImage) -> None:
        self.image = image
        for sector in range(16, 64):
            pvd = image.read(sector * SECTOR_SIZE, SECTOR_SIZE)
            if pvd[1:6] == b"CD001" and pvd[0] == 1:
                self.root = self._parse_record(pvd, 156)
                return
        raise ImageError("no ISO9660 primary volume descriptor")

    def _parse_record(self, data: bytes, offset: int) -> Entry:
        length = data[offset]
        if length == 0:
            raise ImageError("empty directory record")
        extent = _both_endian_u32(data, offset + 2)
        size = _both_endian_u32(data, offset + 10)
        flags = data[offset + 25]
        name_len = data[offset + 32]
        raw_name = data[offset + 33 : offset + 33 + name_len]
        if raw_name in (b"\x00", b"\x01"):
            name = "." if raw_name == b"\x00" else ".."
        else:
            name = raw_name.decode("ascii", "replace").split(";", 1)[0]
        return Entry(name=name, extent=extent, size=size, is_dir=bool(flags & 0x02))

    def entries(self, directory: Entry) -> list[Entry]:
        if not directory.is_dir:
            raise ImageError(f"{directory.name} is not a directory")
        data = self.image.read(directory.extent * SECTOR_SIZE, directory.size)
        found: list[Entry] = []
        offset = 0
        while offset < len(data):
            length = data[offset]
            if length == 0:
                # Records never cross a sector boundary; the rest is padding.
                offset = (offset // SECTOR_SIZE + 1) * SECTOR_SIZE
                continue
            entry = self._parse_record(data, offset)
            assert isinstance(entry, Entry)
            if entry.name not in (".", ".."):
                found.append(entry)
            offset += length
        return found

    def find(self, *parts: str) -> Entry:
        directory = self.root
        for i, part in enumerate(parts):
            wanted = part.upper()
            matches = [e for e in self.entries(directory) if e.name.upper() == wanted]
            if not matches:
                raise ImageError("/".join(parts) + " not found in the image")
            directory = matches[0]
            if i + 1 < len(parts) and not directory.is_dir:
                raise ImageError("/".join(parts[: i + 1]) + " is not a directory")
        return directory

    def read_file(self, entry: Entry) -> bytes:
        if entry.is_dir:
            raise ImageError(f"{entry.name} is a directory")
        return self.image.read(entry.extent * SECTOR_SIZE, entry.size)


def read_sfo_title(sfo: bytes) -> str | None:
    """Return TITLE from a PARAM.SFO, without materializing the whole format."""
    if len(sfo) < 20 or sfo[:4] != b"\x00PSF":
        return None
    key_table, data_table, count = struct.unpack_from("<III", sfo, 8)
    entries = [struct.unpack_from("<HHIII", sfo, 20 + i * 16) for i in range(count)]
    for key_offset, _fmt, _length, _max_length, data_offset in entries:
        start = key_table + key_offset
        end = sfo.find(b"\x00", start)
        if end < 0:
            continue
        key = sfo[start:end].decode("utf-8", "replace")
        if key != "TITLE":
            continue
        value_start = data_table + data_offset
        value_end = sfo.find(b"\x00", value_start)
        if value_end < 0:
            value_end = len(sfo)
        return sfo[value_start:value_end].decode("utf-8", "replace")
    return None


def open_image(path: Path) -> tuple[DiscImage, Iso9660]:
    with path.open("rb") as probe:
        magic = probe.read(4)
    image: DiscImage = CisoImage(path) if magic == b"CISO" else DiscImage(path)
    try:
        return image, Iso9660(image)
    except Exception:
        image.close()
        raise


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image", type=Path, help="PSP ISO or CISO image")
    parser.add_argument("output", type=Path, help="destination for DATA.BIN")
    parser.add_argument(
        "--force",
        action="store_true",
        help="overwrite the destination if it already exists",
    )
    args = parser.parse_args(argv)

    if args.output.exists() and not args.force:
        print(f"error: {args.output} exists (use --force)", file=sys.stderr)
        return 1

    image, iso = open_image(args.image)
    try:
        update = iso.find(*UPDATE_DIR)
        data = iso.find(*UPDATE_DIR, DATA_BIN)
        payload = iso.read_file(data)
        try:
            title = read_sfo_title(iso.read_file(iso.find(*UPDATE_DIR, PARAM_SFO)))
        except ImageError:
            title = None
    finally:
        image.close()

    if payload[:4] != b"PSAR":
        print(f"error: {DATA_BIN} does not start with PSAR", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)
    print(f"update directory: {update.name}")
    print(f"firmware: {title or 'unknown'}")
    print(f"wrote {args.output} ({len(payload)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

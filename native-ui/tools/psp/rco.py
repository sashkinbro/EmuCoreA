"""Reader for the PSP RCO (PRF) resource containers in flash0:/vsh/resource.

The container is a tree of 0x28-byte entries plus type-specific fields. The tree
area is either stored plainly, zlib-compressed or RLZ-compressed; image and
sound payloads are addressed from separate file areas and may be compressed on
their own.

Only the parts needed to take the XMB's images and sounds out are implemented:
entry walking, labels, images (GIM) and sounds (VAG). Text, objects and
animations are walked and reported but not interpreted yet.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

from . import rlz

MAGIC = 0x46525000  # "\0PRF"
HEADER_SIZE = 0xA4
ENTRY_HEADER_SIZE = 0x28
NONE = 0xFFFFFFFF

TABLE_MAIN = 1
TABLE_VSMX = 2
TABLE_TEXT = 3
TABLE_IMAGE = 4
TABLE_MODEL = 5
TABLE_SOUND = 6
TABLE_FONT = 7
TABLE_OBJECT = 8
TABLE_ANIM = 9

TABLE_NAMES = {
    TABLE_MAIN: "main",
    TABLE_VSMX: "vsmx",
    TABLE_TEXT: "text",
    TABLE_IMAGE: "image",
    TABLE_MODEL: "model",
    TABLE_SOUND: "sound",
    TABLE_FONT: "font",
    TABLE_OBJECT: "object",
    TABLE_ANIM: "anim",
}

COMPRESSION_NONE = 0
COMPRESSION_ZLIB = 1
COMPRESSION_RLZ = 2

IMAGE_FORMATS = {
    0: "png", 1: "jpeg", 2: "tiff", 3: "gif", 4: "bmp", 5: "gim",
}


class RcoError(ValueError):
    pass


@dataclass
class Entry:
    offset: int
    type_id: int
    label_offset: int
    head_size: int
    entry_size: int
    subentry_count: int
    next_offset: int
    prev_offset: int
    parent_offset: int
    extra: bytes = b""
    children: list["Entry"] = field(default_factory=list)

    @property
    def table_id(self) -> int:
        return (self.type_id >> 8) & 0xFF

    @property
    def type(self) -> int:
        return self.type_id & 0xFF


@dataclass
class ImageResource:
    label: str
    offset: int
    format: int
    compression: int
    packed_size: int
    unpacked_size: int
    data: bytes

    @property
    def extension(self) -> str:
        return IMAGE_FORMATS.get(self.format, f"fmt{self.format}")


@dataclass
class SoundChannel:
    size: int
    offset: int
    data: bytes


@dataclass
class SoundResource:
    label: str
    offset: int
    format: int
    channels: list[SoundChannel]

    @property
    def size(self) -> int:
        return sum(len(c.data) for c in self.channels)


class Rco:
    def __init__(self, path: Path, data: bytes) -> None:
        self.path = path
        self.data = data
        self._parse_header()
        self._read_labels()
        self.root = self._read_entry(self.header["p_main"])
        self.tables: dict[int, Entry] = {}
        for child in self.root.children:
            if child.type == 0:
                self.tables.setdefault(child.table_id, child)

    # -- header and table area ------------------------------------------------

    def _parse_header(self) -> None:
        data = self.data
        if len(data) < HEADER_SIZE:
            raise RcoError(f"{self.path.name}: shorter than an RCO header")
        if struct.unpack_from("<I", data, 0)[0] != MAGIC:
            raise RcoError(f"{self.path.name}: not a PSP RCO")
        fields = {
            "version": 4, "compression": 12,
            "p_main": 0x10, "p_vsmx": 0x14, "p_text": 0x18, "p_sound": 0x1C,
            "p_model": 0x20, "p_image": 0x24, "p_font": 0x2C,
            "p_object": 0x30, "p_anim": 0x34,
            "p_text_data": 0x38, "l_text_data": 0x3C,
            "p_label_data": 0x40, "l_label_data": 0x44,
            "p_event_data": 0x48, "l_event_data": 0x4C,
            "p_image_data": 0x80, "l_image_data": 0x84,
            "p_sound_data": 0x88, "l_sound_data": 0x8C,
            "p_model_data": 0x90, "l_model_data": 0x94,
        }
        self.header = {name: struct.unpack_from("<I", data, offset)[0] for name, offset in fields.items()}
        self.method = (self.header["compression"] >> 4) & 0xF
        self.table_base = HEADER_SIZE
        self.table_data = data
        if self.method == COMPRESSION_ZLIB:
            packed, unpacked, _ = struct.unpack_from("<III", data, HEADER_SIZE)
            blob = data[HEADER_SIZE + 12:HEADER_SIZE + 12 + packed]
            if len(blob) != packed:
                raise RcoError(f"{self.path.name}: truncated zlib table stream")
            self.table_data = zlib.decompress(blob)
            if len(self.table_data) != unpacked:
                raise RcoError(f"{self.path.name}: zlib table size mismatch")
        elif self.method == COMPRESSION_RLZ:
            packed, unpacked, _ = struct.unpack_from("<III", data, HEADER_SIZE)
            blob = data[HEADER_SIZE + 12:HEADER_SIZE + 12 + packed]
            if len(blob) != packed:
                raise RcoError(f"{self.path.name}: truncated RLZ table stream")
            self.table_data, _ = rlz.decompress(blob, unpacked)
        elif self.method != COMPRESSION_NONE:
            raise RcoError(f"{self.path.name}: unknown table compression {self.method}")
        else:
            self.table_base = 0

    def _read_labels(self) -> None:
        if self.header["p_label_data"] == NONE or not self.header["l_label_data"]:
            self.labels = b""
            return
        # Labels live in the (possibly decompressed) table address space, not
        # in the image/sound data pools at the end of the file.
        self.labels = self._table_read(
            self.header["p_label_data"], self.header["l_label_data"]
        )

    def _label(self, offset: int) -> str:
        if offset in (NONE, 0) or offset >= len(self.labels):
            return ""
        end = self.labels.find(b"\0", offset)
        if end < 0:
            end = len(self.labels)
        return self.labels[offset:end].decode("utf-8", "replace")

    def texts(self) -> dict[int, bytes]:
        """Decode the per-language text data pool (lang -> UTF-8/16 bytes)."""
        start = self.header["p_text_data"]
        total = self.header["l_text_data"]
        if start == NONE or total == 0:
            return {}
        result: dict[int, bytes] = {}
        pos = start
        end = min(len(self.data), start + total)
        while pos + 16 <= end:
            lang, _unknown, next_offset, packed, unpacked = struct.unpack_from(
                "<HHIII", self.data, pos
            )
            payload = self.data[pos + 16:pos + 16 + packed]
            if len(payload) != packed:
                break
            if self.method == COMPRESSION_RLZ:
                block, _ = rlz.decompress(payload, unpacked)
            elif self.method == COMPRESSION_ZLIB:
                block = zlib.decompress(payload)
            else:
                block = payload
            if len(block) == unpacked:
                result[lang] = block
            if next_offset == 0:
                break
            pos = start + next_offset
        return result

    def text_entry_strings(self, entry: Entry) -> dict[str, str]:
        """Resolve the text indexes of one text-table entry to label -> text."""
        if entry.table_id != TABLE_TEXT or entry.type != 1 or len(entry.extra) < 8:
            return {}
        lang, text_format, count = struct.unpack_from("<HHI", entry.extra, 0)
        block = self.texts().get(lang)
        if block is None:
            return {}
        strings: dict[str, str] = {}
        for i in range(min(count, (len(entry.extra) - 8) // 12)):
            label_offset, length, offset = struct.unpack_from("<III", entry.extra, 8 + i * 12)
            if offset + length > len(block):
                continue
            raw = block[offset:offset + length]
            if text_format == 1:
                text = raw.decode("utf-16-le", "replace")
            else:
                text = raw.decode("utf-8", "replace")
            label = self._label(label_offset) or f"text_{i:03d}"
            strings[label] = text.rstrip("\0")
        return strings

    # -- entry tree -----------------------------------------------------------

    def _table_read(self, offset: int, size: int) -> bytes:
        start = offset - self.table_base
        end = start + size
        if start < 0 or end > len(self.table_data):
            raise RcoError(f"{self.path.name}: table read outside decompressed data at {offset:#x}")
        return self.table_data[start:end]

    def _read_u32(self, offset: int) -> int:
        return struct.unpack("<I", self._table_read(offset, 4))[0]

    def _read_u16(self, offset: int) -> int:
        return struct.unpack("<H", self._table_read(offset, 2))[0]

    def _read_entry(self, offset: int, depth: int = 0) -> Entry:
        if depth > 24:
            raise RcoError(f"{self.path.name}: entry tree deeper than 24 levels")
        header = self._table_read(offset, ENTRY_HEADER_SIZE)
        (type_id, _blank, label_offset, head_size, entry_size, subentry_count,
         next_offset, prev_offset, parent_offset, _b0, _b1) = struct.unpack("<HHIIIIIIIII", header)
        entry = Entry(
            offset=offset, type_id=type_id, label_offset=label_offset,
            head_size=head_size, entry_size=entry_size,
            subentry_count=subentry_count, next_offset=next_offset,
            prev_offset=prev_offset, parent_offset=parent_offset,
        )
        extra_size = self._extra_size(entry)
        if extra_size:
            entry.extra = self._table_read(offset + ENTRY_HEADER_SIZE, extra_size)
        if subentry_count:
            position = offset + ENTRY_HEADER_SIZE + extra_size
            for index in range(subentry_count):
                child = self._read_entry(position, depth + 1)
                entry.children.append(child)
                if index + 1 < subentry_count:
                    if child.next_offset == 0:
                        break
                    position = child.offset + child.next_offset
        return entry

    def _extra_size(self, entry: Entry) -> int:
        table = entry.table_id
        if entry.type == 0:
            return 0
        if table in (TABLE_IMAGE, TABLE_MODEL):
            if entry.type != 1:
                return 0
            # The packed-size word is absent on short, uncompressed entries.
            if entry.next_offset and entry.next_offset < ENTRY_HEADER_SIZE + 0x10:
                return 0x0C
            return 0x10
        if table == TABLE_SOUND and entry.type == 1:
            channels = self._read_u16(entry.offset + ENTRY_HEADER_SIZE + 2)
            # The entry always carries two channel slots, even for mono.
            return 0x0C + max(2, channels) * 8
        if table == TABLE_TEXT and entry.type == 1:
            count = self._read_u32(entry.offset + ENTRY_HEADER_SIZE + 4)
            return 0x08 + count * 12
        if table == TABLE_FONT and entry.type == 1:
            return 0x0C
        if table == TABLE_OBJECT and entry.type > 0:
            if entry.entry_size >= ENTRY_HEADER_SIZE:
                return entry.entry_size - ENTRY_HEADER_SIZE
            if entry.next_offset >= ENTRY_HEADER_SIZE:
                return entry.next_offset - ENTRY_HEADER_SIZE
            return 0
        if table == TABLE_ANIM and entry.type > 1 and entry.next_offset >= ENTRY_HEADER_SIZE:
            return entry.next_offset - ENTRY_HEADER_SIZE
        return 0

    # -- resources ------------------------------------------------------------

    def iter_entries(self, table: int | None = None):
        for node in self._walk(self.root):
            if table is None or node.table_id == table:
                yield node

    def _walk(self, entry: Entry):
        yield entry
        for child in entry.children:
            yield from self._walk(child)

    def table_children(self, table: int) -> list[Entry]:
        node = self.tables.get(table)
        return node.children if node else []

    def images(self) -> list[ImageResource]:
        data_base = self.header["p_image_data"]
        out: list[ImageResource] = []
        for entry in self.table_children(TABLE_IMAGE):
            if entry.type != 1 or len(entry.extra) < 12:
                continue
            image_format, compression, packed_size, offset = struct.unpack_from("<HHII", entry.extra)
            unpacked_size = packed_size
            if compression and len(entry.extra) >= 16:
                unpacked_size = struct.unpack_from("<I", entry.extra, 12)[0]
            if packed_size == 0 or data_base == NONE:
                continue
            start = data_base + offset
            end = start + packed_size
            if end > len(self.data):
                raise RcoError(f"{self.path.name}: image payload outside the file")
            payload = self.data[start:end]
            if compression == COMPRESSION_ZLIB:
                payload = zlib.decompress(payload)
            elif compression == COMPRESSION_RLZ:
                payload, _ = rlz.decompress(payload, unpacked_size)
            elif compression != COMPRESSION_NONE:
                raise RcoError(f"{self.path.name}: image compression {compression}")
            out.append(ImageResource(
                label=self._label(entry.label_offset) or f"image_{offset:06x}",
                offset=offset, format=image_format, compression=compression,
                packed_size=packed_size, unpacked_size=unpacked_size, data=payload,
            ))
        return out

    def sounds(self) -> list[SoundResource]:
        data_base = self.header["p_sound_data"]
        out: list[SoundResource] = []
        for entry in self.table_children(TABLE_SOUND):
            if entry.type != 1 or len(entry.extra) < 12:
                continue
            sound_format, channels, _total, base_offset = struct.unpack_from("<HHII", entry.extra)
            channel_data: list[SoundChannel] = []
            for index in range(min(channels, 2)):
                size, offset = struct.unpack_from("<II", entry.extra, 12 + index * 8)
                if size == 0 or data_base == NONE:
                    continue
                start = data_base + offset
                end = start + size
                if end > len(self.data):
                    raise RcoError(f"{self.path.name}: sound payload outside the file")
                channel_data.append(SoundChannel(size=size, offset=offset, data=self.data[start:end]))
            out.append(SoundResource(
                label=self._label(entry.label_offset) or f"sound_{base_offset:06x}",
                offset=base_offset, format=sound_format, channels=channel_data,
            ))
        return out

    def models(self) -> list[ImageResource]:
        """Model payloads (GMO files) use the same entry layout as images."""
        data_base = self.header["p_model_data"]
        out: list[ImageResource] = []
        for entry in self.table_children(TABLE_MODEL):
            if entry.type != 1 or len(entry.extra) < 12:
                continue
            model_format, compression, packed_size, offset = struct.unpack_from("<HHII", entry.extra)
            unpacked_size = packed_size
            if compression and len(entry.extra) >= 16:
                unpacked_size = struct.unpack_from("<I", entry.extra, 12)[0]
            if packed_size == 0 or data_base == NONE:
                continue
            start = data_base + offset
            end = start + packed_size
            if end > len(self.data):
                raise RcoError(f"{self.path.name}: model payload outside the file")
            payload = self.data[start:end]
            if compression == COMPRESSION_ZLIB:
                payload = zlib.decompress(payload)
            elif compression == COMPRESSION_RLZ:
                payload, _ = rlz.decompress(payload, unpacked_size)
            elif compression != COMPRESSION_NONE:
                raise RcoError(f"{self.path.name}: model compression {compression}")
            out.append(ImageResource(
                label=self._label(entry.label_offset) or f"model_{offset:06x}",
                offset=offset, format=model_format, compression=compression,
                packed_size=packed_size, unpacked_size=unpacked_size, data=payload,
            ))
        return out


def open_rco(path: Path) -> Rco:
    return Rco(path, path.read_bytes())


def summary(rco: Rco) -> str:
    parts = [f"{rco.path.name}: v{rco.header['version']:#x} method={rco.method}"]
    for table, node in sorted(rco.tables.items()):
        parts.append(f"  {TABLE_NAMES.get(table, table)}: {len(node.children)} entries")
    parts.append(f"  images: {len(rco.images())}, sounds: {len(rco.sounds())}")
    return "\n".join(parts)

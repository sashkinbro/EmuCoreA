# SPDX-FileCopyrightText: 2026 SBRO
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reader for the PSP PGF fonts in flash0:/font.

PGF stores a char map, glyph metadata tables and a nibble-RLE bitmap stream per
glyph. The layout and decoding here follow the firmware format as implemented
by PPSSPP's sceFont code; the output is deliberately the raw 4-bit alpha of each
glyph so callers can choose their own atlas format.

This is offline tooling: it never ships in the application.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"PGF0"
HEADER_SIZE = 0x188


class PgfError(ValueError):
    pass


@dataclass(frozen=True)
class Glyph:
    width: int
    height: int
    left: int
    top: int
    flags: int
    shadow_flags: int
    shadow_id: int
    advance_h: int
    advance_v: int
    dimension_width: int
    dimension_height: int
    x_adjust_h: int
    x_adjust_v: int
    y_adjust_h: int
    y_adjust_v: int
    pointer: int
    pixels: bytes  # width * height, 4-bit alpha values (0..15)


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _s32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def _get_bits(data: bytes, bit_pos: int, count: int) -> int:
    value = 0
    for i in range(count):
        value |= ((data[(bit_pos + i) >> 3] >> ((bit_pos + i) & 7)) & 1) << i
    return value


def _consume_bits(data: bytes, bit_pos: int, count: int) -> tuple[int, int]:
    return _get_bits(data, bit_pos, count), bit_pos + count


def _align_bits(bits: int) -> int:
    return (bits + 31) & ~31


class Pgf:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        if len(self.data) < HEADER_SIZE:
            raise PgfError(f"{path.name}: shorter than a PGF header")
        if self.data[4:8] != MAGIC:
            raise PgfError(f"{path.name}: not a PGF font")
        self.revision = _s32(self.data, 0x08)
        self.char_map_length = _s32(self.data, 0x10)
        self.char_pointer_length = _s32(self.data, 0x14)
        self.char_map_bpe = _s32(self.data, 0x18)
        self.char_pointer_bpe = _s32(self.data, 0x1C)
        self.bpp = self.data[0x22]
        self.font_name = self.data[0x35:0x75].split(b"\0", 1)[0].decode("ascii", "replace")
        self.first_glyph = _u16(self.data, 0xB6)
        self.last_glyph = _u16(self.data, 0xB8)
        self.dim_table_length = self.data[0x102]
        self.x_adjust_table_length = self.data[0x103]
        self.y_adjust_table_length = self.data[0x104]
        self.advance_table_length = self.data[0x105]
        self.shadow_map_length = _s32(self.data, 0x16C)
        self.shadow_map_bpe = _s32(self.data, 0x170)
        if self.revision not in (2, 3):
            raise PgfError(f"unsupported PGF revision {self.revision}")
        self._parse()
        self._glyph_cache: dict[int, Glyph] = {}

    def _parse(self) -> None:
        pos = HEADER_SIZE
        if self.revision == 3:
            self.comp_length_1 = _s32(self.data, pos)
            self.comp_length_2 = _s32(self.data, pos + 8)
            pos += 20
        else:
            self.comp_length_1 = self.comp_length_2 = 0

        def read_table(count: int) -> tuple[list[int], list[int]]:
            nonlocal pos
            first = [0] * count
            second = [0] * count
            for i in range(count):
                first[i] = _u32(self.data, pos)
                second[i] = _u32(self.data, pos + 4)
                pos += 8
            return first, second

        self.dimension_table = read_table(self.dim_table_length)
        self.x_adjust_table = read_table(self.x_adjust_table_length)
        self.y_adjust_table = read_table(self.y_adjust_table_length)
        self.advance_table = read_table(self.advance_table_length)

        self.shadow_map_bits = _align_bits(self.shadow_map_length * self.shadow_map_bpe)
        shadow_map_ptr = pos
        pos += self.shadow_map_bits // 8

        if self.revision == 3:
            pos += (self.comp_length_1 + self.comp_length_2) * 4

        self.char_map_bits = _align_bits(self.char_map_length * self.char_map_bpe)
        char_map_ptr = pos
        pos += self.char_map_bits // 8
        self.char_pointer_bits = _align_bits(self.char_pointer_length * self.char_pointer_bpe)
        char_pointer_ptr = pos
        pos += self.char_pointer_bits // 8

        if pos > len(self.data):
            raise PgfError(f"{self.path.name}: tables run past the file")
        self.font_data = self.data[pos:]

        self.charmap = [
            _get_bits(self.data, char_map_ptr * 8 + i * self.char_map_bpe, self.char_map_bpe)
            for i in range(self.char_map_length)
        ]
        self.char_pointers = [
            _get_bits(self.data, char_pointer_ptr * 8 + i * self.char_pointer_bpe, self.char_pointer_bpe)
            for i in range(self.char_pointer_length)
        ]
        self.shadow_map = [
            _get_bits(self.data, shadow_map_ptr * 8 + i * self.shadow_map_bpe, self.shadow_map_bpe)
            for i in range(self.shadow_map_length)
        ]
        for i, code in enumerate(self.charmap):
            if code >= len(self.char_pointers):
                self.charmap[i] = 0xFFFF

        self.glyphs: list[Glyph | None] = []
        for pointer in self.char_pointers:
            if pointer < 0:
                self.glyphs.append(None)
            else:
                glyph = self._read_char(pointer * 4 * 8)
                self.glyphs.append(glyph)

        self.shadow_glyphs: list[Glyph | None] = []
        for glyph in self.glyphs:
            entry = None
            if glyph is not None and glyph.shadow_id < len(self.shadow_map):
                char_id = self.shadow_map[glyph.shadow_id]
                if char_id < len(self.glyphs) and self.char_pointers[char_id] >= 0:
                    entry = self._read_shadow(self.char_pointers[char_id] * 4 * 8)
            self.shadow_glyphs.append(entry)

    def _skip_glyph_header(self, bit_pos: int) -> tuple[dict[str, int], int]:
        bit_pos += 14  # declared glyph size
        width, bit_pos = _consume_bits(self.font_data, bit_pos, 7)
        height, bit_pos = _consume_bits(self.font_data, bit_pos, 7)
        left, bit_pos = _consume_bits(self.font_data, bit_pos, 7)
        top, bit_pos = _consume_bits(self.font_data, bit_pos, 7)
        if left >= 64:
            left -= 128
        if top >= 64:
            top -= 128
        flags, bit_pos = _consume_bits(self.font_data, bit_pos, 6)
        shadow_flags, bit_pos = _consume_bits(self.font_data, bit_pos, 2)
        shadow_flags = (shadow_flags << 5)
        part, bit_pos = _consume_bits(self.font_data, bit_pos, 2)
        shadow_flags |= part << 3
        part, bit_pos = _consume_bits(self.font_data, bit_pos, 3)
        shadow_flags |= part
        shadow_id, bit_pos = _consume_bits(self.font_data, bit_pos, 9)
        return {
            "width": width, "height": height, "left": left, "top": top,
            "flags": flags, "shadow_flags": shadow_flags, "shadow_id": shadow_id,
        }, bit_pos

    def _read_char(self, bit_pos: int) -> Glyph:
        fields, bit_pos = self._skip_glyph_header(bit_pos)
        flags = fields["flags"]
        if flags & 0x04:
            index, bit_pos = _consume_bits(self.font_data, bit_pos, 8)
            dimension_width = self.dimension_table[0][index] if index < self.dim_table_length else 0
            dimension_height = self.dimension_table[1][index] if index < self.dim_table_length else 0
        else:
            dimension_width, bit_pos = _consume_bits(self.font_data, bit_pos, 32)
            dimension_height, bit_pos = _consume_bits(self.font_data, bit_pos, 32)
        if flags & 0x08:
            index, bit_pos = _consume_bits(self.font_data, bit_pos, 8)
            x_adjust_h = self.x_adjust_table[0][index] if index < self.x_adjust_table_length else 0
            x_adjust_v = self.x_adjust_table[1][index] if index < self.x_adjust_table_length else 0
        else:
            x_adjust_h, bit_pos = _consume_bits(self.font_data, bit_pos, 32)
            x_adjust_v, bit_pos = _consume_bits(self.font_data, bit_pos, 32)
        if flags & 0x10:
            index, bit_pos = _consume_bits(self.font_data, bit_pos, 8)
            y_adjust_h = self.y_adjust_table[0][index] if index < self.y_adjust_table_length else 0
            y_adjust_v = self.y_adjust_table[1][index] if index < self.y_adjust_table_length else 0
        else:
            y_adjust_h, bit_pos = _consume_bits(self.font_data, bit_pos, 32)
            y_adjust_v, bit_pos = _consume_bits(self.font_data, bit_pos, 32)
        if flags & 0x20:
            index, bit_pos = _consume_bits(self.font_data, bit_pos, 8)
            advance_h = self.advance_table[0][index] if index < self.advance_table_length else 0
            advance_v = self.advance_table[1][index] if index < self.advance_table_length else 0
        else:
            advance_h, bit_pos = _consume_bits(self.font_data, bit_pos, 32)
            advance_v, bit_pos = _consume_bits(self.font_data, bit_pos, 32)
        pointer = bit_pos // 8
        pixels = self._decode_bitmap(pointer * 8, fields["width"], fields["height"])
        if (flags & 0x03) == 0x02:
            # V_ROWS stores the run stream column by column.
            pixels = bytes(
                pixels[x * fields["height"] + y]
                for y in range(fields["height"])
                for x in range(fields["width"])
            )
        return Glyph(
            width=fields["width"], height=fields["height"], left=fields["left"], top=fields["top"],
            flags=flags, shadow_flags=fields["shadow_flags"], shadow_id=fields["shadow_id"],
            advance_h=advance_h, advance_v=advance_v,
            dimension_width=dimension_width, dimension_height=dimension_height,
            x_adjust_h=x_adjust_h, x_adjust_v=x_adjust_v,
            y_adjust_h=y_adjust_h, y_adjust_v=y_adjust_v,
            pointer=pointer, pixels=pixels,
        )

    def _read_shadow(self, bit_pos: int) -> Glyph:
        glyph = self._read_char(bit_pos)
        skip, _ = _consume_bits(self.font_data, bit_pos, 14)
        bit_pos += skip * 8
        # Shadow sub-glyph: 14-bit size, then width/height/left/top; the bitmap
        # pointer follows immediately, with no flag or metric block.
        bit_pos += 14
        width, bit_pos = _consume_bits(self.font_data, bit_pos, 7)
        height, bit_pos = _consume_bits(self.font_data, bit_pos, 7)
        left, bit_pos = _consume_bits(self.font_data, bit_pos, 7)
        top, bit_pos = _consume_bits(self.font_data, bit_pos, 7)
        if left >= 64:
            left -= 128
        if top >= 64:
            top -= 128
        pointer = bit_pos // 8
        pixels = self._decode_bitmap(pointer * 8, width, height)
        if (glyph.flags & 0x03) == 0x02 and width > 0 and height > 0:
            pixels = bytes(
                pixels[x * height + y]
                for y in range(height)
                for x in range(width)
            )
        return Glyph(
            width=width, height=height, left=left, top=top,
            flags=glyph.flags, shadow_flags=glyph.shadow_flags, shadow_id=glyph.shadow_id,
            advance_h=glyph.advance_h, advance_v=glyph.advance_v,
            dimension_width=glyph.dimension_width, dimension_height=glyph.dimension_height,
            x_adjust_h=glyph.x_adjust_h, x_adjust_v=glyph.x_adjust_v,
            y_adjust_h=glyph.y_adjust_h, y_adjust_v=glyph.y_adjust_v,
            pointer=pointer, pixels=pixels,
        )

    def _decode_bitmap(self, bit_pos: int, width: int, height: int) -> bytes:
        count_pixels = width * height
        if count_pixels <= 0:
            return b""
        pixels = bytearray()
        bit_limit = len(self.font_data) * 8
        while len(pixels) < count_pixels and bit_pos + 8 < bit_limit:
            nibble, bit_pos = _consume_bits(self.font_data, bit_pos, 4)
            value = 0
            if nibble < 8:
                value, bit_pos = _consume_bits(self.font_data, bit_pos, 4)
                run = nibble + 1
            else:
                run = 16 - nibble
            for _ in range(run):
                if len(pixels) >= count_pixels:
                    break
                if nibble >= 8:
                    value, bit_pos = _consume_bits(self.font_data, bit_pos, 4)
                pixels.append(value)
        if len(pixels) < count_pixels:
            raise PgfError(f"{self.path.name}: glyph bitmap ended early")
        return bytes(pixels)

    def glyph(self, char: str, shadow: bool = False) -> Glyph | None:
        code = ord(char) if len(char) == 1 else int(char)
        cached = self._glyph_cache.get(code)
        if cached is not None and not shadow:
            return cached
        if code < self.first_glyph:
            return None
        index = code - self.first_glyph
        if index < len(self.charmap):
            index = self.charmap[index]
        table = self.shadow_glyphs if shadow else self.glyphs
        if 0 <= index < len(table):
            glyph = table[index]
            if glyph is not None and not shadow:
                self._glyph_cache[code] = glyph
            return glyph
        return None

    def text_width(self, text: str) -> float:
        total = 0
        for char in text:
            glyph = self.glyph(char)
            if glyph is not None:
                total += glyph.advance_h
        return total / 64.0

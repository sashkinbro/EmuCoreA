"""Decoder for PSP GIM images, as used by the XMB's RCO resources.

The container after "MIG.00.1PSP" is a chain of 16-byte block headers (type,
size including the header, distance to the next sibling, data offset). An image
block (type 4) starts with a 0x30-byte image structure: format, pixel order,
width, height, bits per pixel, pitch/height alignment, then the offset of the
frame and of its index. Indexed images have a sibling palette block (type 5)
holding a 256x1 RGBA8888 image.

Formats are the ones gimconv assigns: 0 RGBA5650, 1 RGBA5551, 2 RGBA4444,
3 RGBA8888, 4 INDEX4, 5 INDEX8, 8/9/10 DXT1/3/5. Only little-endian PSP files
are accepted; the PS3 big-endian variant is a different consumer's problem.

Pixel order 1 is the PSP's tiled layout: data is stored in 16x8 tiles for
8-bit pixels (tile width is 128 bits / bpp), the image is padded to whole
tiles, and rows are padded to the pitch alignment.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

MAGIC = b"MIG.00.1PSP\0"

BLOCK_FILEINFO = 0xFF
BLOCK_ROOT = 2
BLOCK_PICTURE = 3
BLOCK_IMAGE = 4
BLOCK_PALETTE = 5


class GimError(ValueError):
    pass


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    format: int
    pixel_order: int
    bpp: int
    pixels: bytes  # RGBA8888, width * height * 4
    palette: bytes | None = None


@dataclass(frozen=True)
class _Header:
    block_type: int
    size: int
    next: int
    data_offset: int


def _u16(data: bytes, offset: int) -> int:
    if offset + 2 > len(data):
        raise GimError("GIM read outside the file")
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    if offset + 4 > len(data):
        raise GimError("GIM read outside the file")
    return struct.unpack_from("<I", data, offset)[0]


def _read_header(data: bytes, offset: int) -> _Header:
    if offset + 16 > len(data):
        raise GimError("GIM block header outside the file")
    block_type, _pad, size, next_block, data_offset = struct.unpack_from("<HHIII", data, offset)
    if size < 16 or offset + size > len(data) or data_offset < 16 or data_offset > size:
        raise GimError(f"invalid GIM block at {offset:#x}")
    return _Header(block_type, size, next_block, data_offset)


def _decode_image_block(data: bytes, block_start: int, header: _Header) -> tuple[Image, bytes | None]:
    base = block_start + header.data_offset
    if base + 0x30 > len(data):
        raise GimError("GIM image structure outside the file")
    structure_size = _u16(data, base)
    if structure_size != 0x30:
        raise GimError(f"unexpected GIM image structure size {structure_size:#x}")
    image_format = _u16(data, base + 4)
    pixel_order = _u16(data, base + 6)
    width = _u16(data, base + 8)
    height = _u16(data, base + 10)
    bpp = _u16(data, base + 12)
    pitch_align = max(1, _u16(data, base + 14))
    next_index = _u32(data, base + 0x18)
    frame_start = _u32(data, base + 0x1C)
    frame_end = _u32(data, base + 0x20)
    level_count = _u16(data, base + 0x2A)
    frame_count = _u16(data, base + 0x2E)
    if frame_count > 1 or level_count > 1:
        raise GimError("multi-frame or mipmapped GIM is not supported yet")
    if width == 0 or height == 0:
        raise GimError("GIM image has no pixels")

    index_at = block_start + header.data_offset + next_index
    frame_offset = _u32(data, index_at)
    start = block_start + header.data_offset + frame_offset
    end = block_start + header.data_offset + frame_end
    if end > len(data) or start >= end:
        raise GimError("GIM frame outside the block")
    frame = data[start:end]

    palette = None
    return _decode_pixels(frame, width, height, image_format, pixel_order, bpp, pitch_align), palette


def _decode_pixels(frame: bytes, width: int, height: int, image_format: int, pixel_order: int,
                   bpp: int, pitch_align: int) -> Image:
    tile_width = 128 // bpp if bpp else 1
    tile_height = 8
    if pixel_order == 1:
        stored_w = (width + tile_width - 1) // tile_width * tile_width
        stored_h = (height + tile_height - 1) // tile_height * tile_height
    else:
        stored_w, stored_h = width, height

    bytes_per_pixel = (bpp + 7) // 8
    row_bytes = (stored_w * bpp + 7) // 8
    pitch = row_bytes
    if pitch % pitch_align:
        pitch += pitch_align - pitch % pitch_align
    needed = pitch * stored_h
    if len(frame) < needed:
        raise GimError(f"GIM frame is {len(frame)} bytes, {needed} needed for {stored_w}x{stored_h}")

    def read_value(stored_x: int, stored_y: int) -> int:
        row_start = stored_y * pitch
        if bpp == 4:
            return (frame[row_start + stored_x // 2] >> (4 * (stored_x & 1))) & 0xF
        at = row_start + stored_x * bytes_per_pixel
        if bpp == 8:
            return frame[at]
        if bpp == 16:
            return frame[at] | (frame[at + 1] << 8)
        return struct.unpack_from("<I", frame, at)[0]

    values = bytearray(width * height * 4)
    tiles_per_row = stored_w // tile_width if pixel_order == 1 else 0
    tile_pixels = tile_width * tile_height
    for y in range(height):
        for x in range(width):
            if pixel_order == 1:
                tile_x, tile_y = x // tile_width, y // tile_height
                within = (y % tile_height) * tile_width + (x % tile_width)
                position = (tile_y * tiles_per_row + tile_x) * tile_pixels + within
                stored_x, stored_y = position % stored_w, position // stored_w
            else:
                stored_x, stored_y = x, y
            _write_pixel(values, (y * width + x) * 4, read_value(stored_x, stored_y), image_format)
    return Image(width, height, image_format, pixel_order, bpp, bytes(values))


def _write_pixel(out: bytearray, at: int, value: int, image_format: int) -> None:
    if image_format == 3:  # RGBA8888
        out[at] = value & 0xFF
        out[at + 1] = (value >> 8) & 0xFF
        out[at + 2] = (value >> 16) & 0xFF
        out[at + 3] = (value >> 24) & 0xFF
    elif image_format == 2:  # RGBA4444
        out[at] = ((value & 0xF) * 17)
        out[at + 1] = (((value >> 4) & 0xF) * 17)
        out[at + 2] = (((value >> 8) & 0xF) * 17)
        out[at + 3] = (((value >> 12) & 0xF) * 17)
    elif image_format == 1:  # RGBA5551
        r, g, b, a = value & 0x1F, (value >> 5) & 0x1F, (value >> 10) & 0x1F, (value >> 15) & 1
        out[at] = (r << 3) | (r >> 2)
        out[at + 1] = (g << 3) | (g >> 2)
        out[at + 2] = (b << 3) | (b >> 2)
        out[at + 3] = 255 if a else 0
    elif image_format == 0:  # RGBA5650
        r, g, b = value & 0x1F, (value >> 5) & 0x3F, (value >> 11) & 0x1F
        out[at] = (r << 3) | (r >> 2)
        out[at + 1] = (g << 2) | (g >> 4)
        out[at + 2] = (b << 3) | (b >> 2)
        out[at + 3] = 255
    else:
        # Indexed formats keep their index in the red channel until the
        # palette is applied; callers must combine the two images.
        out[at] = value & 0xFF
        out[at + 1] = (value >> 8) & 0xFF
        out[at + 2] = 0
        out[at + 3] = 0xFF


def decode(data: bytes) -> Image:
    """Decode the first picture in a PSP GIM file."""
    if len(data) < 0x20 or not data.startswith(b"MIG.00.1PSP"):
        raise GimError("not a little-endian PSP GIM")
    image: Image | None = None
    palette_values: list[int] | None = None
    offset = 16
    guard = 0
    while offset + 16 <= len(data) and guard < 512:
        guard += 1
        header = _read_header(data, offset)
        if header.block_type == BLOCK_IMAGE:
            image = _decode_image_block(data, offset, header)[0]
        elif header.block_type == BLOCK_PALETTE:
            palette = _decode_image_block(data, offset, header)[0]
            if palette.format != 3:
                raise GimError(f"unsupported GIM palette format {palette.format}")
            palette_values = []
            for index in range(palette.width * palette.height):
                at = index * 4
                palette_values.append((
                    palette.pixels[at], palette.pixels[at + 1],
                    palette.pixels[at + 2], palette.pixels[at + 3],
                ))
        if header.next == 0:
            break
        offset += header.next
    if image is None:
        raise GimError("GIM contains no image")
    if image.format in (4, 5):
        if palette_values is None:
            raise GimError("indexed GIM without a palette")
        if image.format == 4:
            # 4-bit indices were expanded by _write_pixel into the low nibble.
            indices = [image.pixels[i * 4] for i in range(image.width * image.height)]
        else:
            indices = [image.pixels[i * 4] for i in range(image.width * image.height)]
        rgba = bytearray(image.width * image.height * 4)
        for i, index in enumerate(indices):
            if index >= len(palette_values):
                raise GimError(f"palette index {index} outside {len(palette_values)} entries")
            rgba[i * 4:i * 4 + 4] = bytes(palette_values[index])
        image = Image(image.width, image.height, 3, image.pixel_order, 32, bytes(rgba), bytes(rgba))
    return image


def info(data: bytes) -> dict:
    """Return format and dimensions without decoding pixels."""
    if len(data) < 0x20 or not data.startswith(b"MIG.00.1PSP"):
        raise GimError("not a little-endian PSP GIM")
    offset = 16
    for _ in range(512):
        header = _read_header(data, offset)
        if header.block_type == BLOCK_IMAGE:
            base = offset + header.data_offset
            return {
                "format": _u16(data, base + 4),
                "pixel_order": _u16(data, base + 6),
                "width": _u16(data, base + 8),
                "height": _u16(data, base + 10),
                "bpp": _u16(data, base + 12),
            }
        if header.next == 0:
            break
        offset += header.next
    raise GimError("GIM contains no image")


def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def png_bytes(image: Image) -> bytes:
    """Encode decoded RGBA pixels as a PNG (no external imaging library)."""
    rows = bytearray()
    stride = image.width * 4
    for y in range(image.height):
        rows.append(0)
        rows += image.pixels[y * stride:(y + 1) * stride]
    header = struct.pack(">IIBBBBB", image.width, image.height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n"
            + _png_chunk(b"IHDR", header)
            + _png_chunk(b"IDAT", zlib.compress(bytes(rows), 9))
            + _png_chunk(b"IEND", b""))

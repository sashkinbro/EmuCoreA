// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

#include "psp_formats.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// Port of native-ui/tools/psp/gim.py (decode + png_bytes).

namespace xmb {

namespace {

constexpr uint16_t kBlockImage = 4;
constexpr uint16_t kBlockPalette = 5;

struct Header {
	uint16_t block_type;
	uint32_t size;
	uint32_t next;
	uint32_t data_offset;
};

struct RawImage {
	int width = 0;
	int height = 0;
	int format = 0;
	std::vector<uint8_t> px;  // RGBA8888 (indexed formats keep the index in R)
};

std::string Hex(uint64_t v) {
	char buf[24];
	snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
	return buf;
}

uint32_t U16(const uint8_t *d, size_t n, uint64_t off) {
	if (off + 2 > n)
		throw PspError("GIM read outside the file");
	return (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8);
}

uint32_t U32(const uint8_t *d, size_t n, uint64_t off) {
	if (off + 4 > n)
		throw PspError("GIM read outside the file");
	return (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) | ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
}

Header ReadHeader(const uint8_t *d, size_t n, uint64_t offset) {
	if (offset + 16 > n)
		throw PspError("GIM block header outside the file");
	Header h;
	h.block_type = (uint16_t)U16(d, n, offset);
	h.size = U32(d, n, offset + 4);
	h.next = U32(d, n, offset + 8);
	h.data_offset = U32(d, n, offset + 12);
	if (h.size < 16 || offset + h.size > n || h.data_offset < 16 || h.data_offset > h.size)
		throw PspError("invalid GIM block at " + Hex(offset));
	return h;
}

void WritePixel(uint8_t *out, uint32_t value, int format) {
	switch (format) {
	case 3:
		out[0] = value & 0xFF;
		out[1] = (value >> 8) & 0xFF;
		out[2] = (value >> 16) & 0xFF;
		out[3] = (value >> 24) & 0xFF;
		break;
	case 2:
		out[0] = (uint8_t)((value & 0xF) * 17);
		out[1] = (uint8_t)(((value >> 4) & 0xF) * 17);
		out[2] = (uint8_t)(((value >> 8) & 0xF) * 17);
		out[3] = (uint8_t)(((value >> 12) & 0xF) * 17);
		break;
	case 1: {
		uint32_t r = value & 0x1F, g = (value >> 5) & 0x1F, b = (value >> 10) & 0x1F, a = (value >> 15) & 1;
		out[0] = (uint8_t)((r << 3) | (r >> 2));
		out[1] = (uint8_t)((g << 3) | (g >> 2));
		out[2] = (uint8_t)((b << 3) | (b >> 2));
		out[3] = a ? 255 : 0;
		break;
	}
	case 0: {
		uint32_t r = value & 0x1F, g = (value >> 5) & 0x3F, b = (value >> 11) & 0x1F;
		out[0] = (uint8_t)((r << 3) | (r >> 2));
		out[1] = (uint8_t)((g << 2) | (g >> 4));
		out[2] = (uint8_t)((b << 3) | (b >> 2));
		out[3] = 255;
		break;
	}
	default:
		out[0] = value & 0xFF;
		out[1] = (value >> 8) & 0xFF;
		out[2] = 0;
		out[3] = 0xFF;
		break;
	}
}

RawImage DecodeImageBlock(const uint8_t *d, size_t n, uint64_t block_start, const Header &header) {
	const uint64_t base = block_start + header.data_offset;
	if (base + 0x30 > n)
		throw PspError("GIM image structure outside the file");
	uint32_t structure_size = U16(d, n, base);
	if (structure_size != 0x30)
		throw PspError("unexpected GIM image structure size " + Hex(structure_size));
	const int format = (int)U16(d, n, base + 4);
	const uint32_t pixel_order = U16(d, n, base + 6);
	const uint32_t width = U16(d, n, base + 8);
	const uint32_t height = U16(d, n, base + 10);
	const uint32_t bpp = U16(d, n, base + 12);
	const uint32_t pitch_align = std::max<uint32_t>(1, U16(d, n, base + 14));
	const uint32_t next_index = U32(d, n, base + 0x18);
	const uint32_t frame_end = U32(d, n, base + 0x20);
	const uint32_t level_count = U16(d, n, base + 0x2A);
	const uint32_t frame_count = U16(d, n, base + 0x2E);
	if (frame_count > 1 || level_count > 1)
		throw PspError("multi-frame or mipmapped GIM is not supported yet");
	if (width == 0 || height == 0)
		throw PspError("GIM image has no pixels");

	const uint32_t frame_offset = U32(d, n, base + next_index);
	const uint64_t start = base + frame_offset;
	const uint64_t end = base + frame_end;
	if (end > n || start >= end)
		throw PspError("GIM frame outside the block");
	const uint8_t *frame = d + start;
	const uint64_t frame_len = end - start;

	const uint32_t tile_width = bpp ? 128 / bpp : 1;
	const uint32_t tile_height = 8;
	uint64_t stored_w, stored_h;
	if (pixel_order == 1) {
		stored_w = (uint64_t)(width + tile_width - 1) / tile_width * tile_width;
		stored_h = (uint64_t)(height + tile_height - 1) / tile_height * tile_height;
	} else {
		stored_w = width;
		stored_h = height;
	}
	const uint32_t bytes_per_pixel = (bpp + 7) / 8;
	const uint64_t row_bytes = (stored_w * bpp + 7) / 8;
	uint64_t pitch = row_bytes;
	if (pitch % pitch_align)
		pitch += pitch_align - pitch % pitch_align;
	const uint64_t needed = pitch * stored_h;
	if (frame_len < needed) {
		throw PspError("GIM frame is " + std::to_string(frame_len) + " bytes, " + std::to_string(needed) +
		               " needed for " + std::to_string(stored_w) + "x" + std::to_string(stored_h));
	}
	if (bpp != 4 && bpp != 8 && bpp != 16 && bpp != 32)
		throw PspError("unsupported GIM bit depth " + std::to_string(bpp));

	RawImage img;
	img.width = (int)width;
	img.height = (int)height;
	img.format = format;
	img.px.resize((size_t)width * height * 4);
	const uint64_t tiles_per_row = pixel_order == 1 ? stored_w / tile_width : 0;
	const uint64_t tile_pixels = (uint64_t)tile_width * tile_height;
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			uint64_t sx, sy;
			if (pixel_order == 1) {
				uint64_t tx = x / tile_width, ty = y / tile_height;
				uint64_t within = (uint64_t)(y % tile_height) * tile_width + (x % tile_width);
				uint64_t position = (ty * tiles_per_row + tx) * tile_pixels + within;
				sx = position % stored_w;
				sy = position / stored_w;
			} else {
				sx = x;
				sy = y;
			}
			const uint64_t row_start = sy * pitch;
			uint32_t value;
			if (bpp == 4) {
				value = (frame[row_start + sx / 2] >> (4 * (sx & 1))) & 0xF;
			} else {
				const uint8_t *p = frame + row_start + sx * bytes_per_pixel;
				if (bpp == 8)
					value = p[0];
				else if (bpp == 16)
					value = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
				else
					value = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
			}
			WritePixel(&img.px[((size_t)y * width + x) * 4], value, format);
		}
	}
	return img;
}

void PutBe32(std::vector<uint8_t> *v, uint32_t x) {
	v->push_back((uint8_t)(x >> 24));
	v->push_back((uint8_t)(x >> 16));
	v->push_back((uint8_t)(x >> 8));
	v->push_back((uint8_t)x);
}

void PngChunk(std::vector<uint8_t> *out, const char *tag, const uint8_t *payload, size_t len) {
	PutBe32(out, (uint32_t)len);
	size_t crc_start = out->size();
	out->insert(out->end(), tag, tag + 4);
	if (len)
		out->insert(out->end(), payload, payload + len);
	uLong crc = crc32(0L, out->data() + crc_start, (uInt)(out->size() - crc_start));
	PutBe32(out, (uint32_t)crc);
}

}  // namespace

GimImage GimDecode(const uint8_t *d, size_t n) {
	static const char kMagic[] = "MIG.00.1PSP";
	if (n < 0x20 || memcmp(d, kMagic, 11) != 0)
		throw PspError("not a little-endian PSP GIM");
	bool have_image = false, have_palette = false;
	RawImage image;
	std::vector<uint8_t> palette;
	uint64_t offset = 16;
	int guard = 0;
	while (offset + 16 <= n && guard < 512) {
		guard++;
		Header header = ReadHeader(d, n, offset);
		if (header.block_type == kBlockImage) {
			image = DecodeImageBlock(d, n, offset, header);
			have_image = true;
		} else if (header.block_type == kBlockPalette) {
			RawImage p = DecodeImageBlock(d, n, offset, header);
			if (p.format != 3)
				throw PspError("unsupported GIM palette format " + std::to_string(p.format));
			palette = std::move(p.px);
			have_palette = true;
		}
		if (header.next == 0)
			break;
		offset += header.next;
	}
	if (!have_image)
		throw PspError("GIM contains no image");
	GimImage out;
	out.width = image.width;
	out.height = image.height;
	if (image.format == 4 || image.format == 5) {
		if (!have_palette)
			throw PspError("indexed GIM without a palette");
		const size_t entries = palette.size() / 4;
		const size_t count = (size_t)image.width * image.height;
		out.rgba.resize(count * 4);
		for (size_t i = 0; i < count; i++) {
			size_t index = image.px[i * 4];
			if (index >= entries)
				throw PspError("palette index " + std::to_string(index) + " outside " + std::to_string(entries) +
				               " entries");
			memcpy(&out.rgba[i * 4], &palette[index * 4], 4);
		}
	} else {
		out.rgba = std::move(image.px);
	}
	return out;
}

std::vector<uint8_t> PngEncode(int width, int height, const uint8_t *rgba) {
	const size_t stride = (size_t)width * 4;
	std::vector<uint8_t> rows((stride + 1) * height);
	for (int y = 0; y < height; y++) {
		rows[(stride + 1) * y] = 0;
		memcpy(&rows[(stride + 1) * y + 1], rgba + stride * y, stride);
	}
	uLongf bound = compressBound((uLong)rows.size());
	std::vector<uint8_t> packed(bound);
	if (compress2(packed.data(), &bound, rows.data(), (uLong)rows.size(), 9) != Z_OK)
		throw PspError("PNG compression failed");
	packed.resize(bound);

	std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	std::vector<uint8_t> ihdr;
	PutBe32(&ihdr, (uint32_t)width);
	PutBe32(&ihdr, (uint32_t)height);
	const uint8_t tail[5] = {8, 6, 0, 0, 0};
	ihdr.insert(ihdr.end(), tail, tail + 5);
	PngChunk(&out, "IHDR", ihdr.data(), ihdr.size());
	PngChunk(&out, "IDAT", packed.data(), packed.size());
	PngChunk(&out, "IEND", nullptr, 0);
	return out;
}

}  // namespace xmb

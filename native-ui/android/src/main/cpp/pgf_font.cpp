// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

#include "pgf_font.h"

#include <algorithm>
#include <cstring>

#include "png_image.h"

namespace xmb {
namespace {

constexpr size_t kHeaderSize = 0x188;

uint16_t U16(const std::vector<uint8_t> &data, size_t offset) {
	return (uint16_t)(data[offset] | (data[offset + 1] << 8));
}

uint32_t U32(const std::vector<uint8_t> &data, size_t offset) {
	return (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) |
	       ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);
}

int S32(const std::vector<uint8_t> &data, size_t offset) {
	return (int)U32(data, offset);
}

size_t AlignBits(size_t bits) {
	return (bits + 31) & ~(size_t)31;
}

uint32_t BitsFrom(const std::vector<uint8_t> &data, size_t bit_pos, int count) {
	uint32_t value = 0;
	for (int i = 0; i < count; ++i) {
		const size_t position = bit_pos + i;
		value |= ((data[position >> 3] >> (position & 7)) & 1) << i;
	}
	return value;
}

}  // namespace

uint32_t NextCodepoint(const std::string &text, size_t &position) {
	if (position >= text.size()) {
		return 0;
	}
	const uint8_t first = (uint8_t)text[position++];
	if (first < 0x80) {
		return first;
	}
	int extra = 0;
	uint32_t value = 0;
	if ((first & 0xE0) == 0xC0) {
		value = first & 0x1F;
		extra = 1;
	} else if ((first & 0xF0) == 0xE0) {
		value = first & 0x0F;
		extra = 2;
	} else if ((first & 0xF8) == 0xF0) {
		value = first & 0x07;
		extra = 3;
	} else {
		return 0xFFFD;
	}
	for (int i = 0; i < extra; ++i) {
		if (position >= text.size()) {
			return 0xFFFD;
		}
		const uint8_t next = (uint8_t)text[position++];
		if ((next & 0xC0) != 0x80) {
			return 0xFFFD;
		}
		value = (value << 6) | (next & 0x3F);
	}
	return value;
}

bool PgfFont::Load(const std::string &path) {
	std::vector<uint8_t> data;
	if (!ReadFile(path, &data) || data.size() < kHeaderSize) {
		return false;
	}
	if (memcmp(&data[4], "PGF0", 4) != 0 || U16(data, 0x00) != 0 || U16(data, 0x02) != kHeaderSize) {
		return false;
	}
	const int revision = S32(data, 0x08);
	if (revision != 2 && revision != 3) {
		return false;
	}
	const int char_map_length = S32(data, 0x10);
	const int char_pointer_length = S32(data, 0x14);
	const int char_map_bpe = S32(data, 0x18);
	const int char_pointer_bpe = S32(data, 0x1C);
	dim_table_length_ = data[0x102];
	x_adjust_table_length_ = data[0x103];
	y_adjust_table_length_ = data[0x104];
	advance_table_length_ = data[0x105];
	const int shadow_map_length = S32(data, 0x16C);
	const int shadow_map_bpe = S32(data, 0x170);
	first_glyph_ = U16(data, 0xB6);
	const char *raw_name = (const char *)&data[0x35];
	name_.assign(raw_name, strnlen(raw_name, 64));
	if (char_map_length < 0 || char_pointer_length < 0 || shadow_map_length < 0 ||
	    char_map_bpe <= 0 || char_pointer_bpe <= 0 || (shadow_map_length > 0 && shadow_map_bpe <= 0)) {
		return false;
	}

	size_t pos = kHeaderSize;
	int comp_length_1 = 0;
	int comp_length_2 = 0;
	if (revision == 3) {
		if (data.size() < pos + 20) {
			return false;
		}
		comp_length_1 = S32(data, pos + 4);
		comp_length_2 = S32(data, pos + 12);
		pos += 20;
	}

	auto read_table = [&](int count, std::vector<int> table[2]) -> bool {
		table[0].resize(count);
		table[1].resize(count);
		for (int i = 0; i < count; ++i) {
			if (pos + 8 > data.size()) {
				return false;
			}
			table[0][i] = S32(data, pos);
			table[1][i] = S32(data, pos + 4);
			pos += 8;
		}
		return true;
	};
	if (!read_table(dim_table_length_, dimension_table_) ||
	    !read_table(x_adjust_table_length_, x_adjust_table_) ||
	    !read_table(y_adjust_table_length_, y_adjust_table_) ||
	    !read_table(advance_table_length_, advance_table_)) {
		return false;
	}

	const size_t shadow_map_bytes = AlignBits((size_t)shadow_map_length * shadow_map_bpe) / 8;
	const size_t shadow_map_ptr = pos;
	pos += shadow_map_bytes;
	pos += (size_t)(comp_length_1 + comp_length_2) * 4;
	const size_t char_map_bytes = AlignBits((size_t)char_map_length * char_map_bpe) / 8;
	const size_t char_map_ptr = pos;
	pos += char_map_bytes;
	const size_t char_pointer_bytes = AlignBits((size_t)char_pointer_length * char_pointer_bpe) / 8;
	const size_t char_pointer_ptr = pos;
	pos += char_pointer_bytes;
	if (pos > data.size()) {
		return false;
	}
	font_data_.assign(data.begin() + pos, data.end());
	charmap_.resize(char_map_length);
	char_pointers_.resize(char_pointer_length);
	shadow_map_.resize(shadow_map_length);
	for (int i = 0; i < char_map_length; ++i) {
		charmap_[i] = (int)BitsFrom(data, char_map_ptr * 8 + (size_t)i * char_map_bpe, char_map_bpe);
		if (charmap_[i] >= char_pointer_length) {
			charmap_[i] = 0xFFFF;
		}
	}
	for (int i = 0; i < char_pointer_length; ++i) {
		char_pointers_[i] = (int)BitsFrom(data, char_pointer_ptr * 8 + (size_t)i * char_pointer_bpe, char_pointer_bpe);
	}
	for (int i = 0; i < shadow_map_length; ++i) {
		shadow_map_[i] = (int)BitsFrom(data, shadow_map_ptr * 8 + (size_t)i * shadow_map_bpe, shadow_map_bpe);
	}

	glyphs_.resize(char_pointer_length);
	shadow_glyphs_.resize(char_pointer_length);
	for (int i = 0; i < char_pointer_length; ++i) {
		if (char_pointers_[i] >= 0) {
			glyphs_[i] = ReadChar((size_t)char_pointers_[i] * 32);
		}
	}
	for (int i = 0; i < char_pointer_length; ++i) {
		if (!glyphs_[i].has_value()) {
			continue;
		}
		const int shadow_id = glyphs_[i]->shadow_id;
		if (shadow_id < 0 || shadow_id >= (int)shadow_map_.size()) {
			continue;
		}
		const int char_id = shadow_map_[shadow_id];
		if (char_id >= 0 && char_id < char_pointer_length && char_pointers_[char_id] >= 0) {
			shadow_glyphs_[char_id] = ReadShadow((size_t)char_pointers_[char_id] * 32);
		}
	}
	return true;
}

uint32_t PgfFont::GetBits(size_t bit_pos, int count) const {
	uint32_t value = 0;
	for (int i = 0; i < count; ++i) {
		const size_t position = bit_pos + i;
		value |= ((font_data_[position >> 3] >> (position & 7)) & 1) << i;
	}
	return value;
}

void PgfFont::SkipGlyphHeader(size_t &bit_pos, FontGlyph &glyph) const {
	bit_pos += 14;
	glyph.width = (int)GetBits(bit_pos, 7); bit_pos += 7;
	glyph.height = (int)GetBits(bit_pos, 7); bit_pos += 7;
	glyph.left = (int)GetBits(bit_pos, 7); bit_pos += 7;
	glyph.top = (int)GetBits(bit_pos, 7); bit_pos += 7;
	if (glyph.left >= 64) glyph.left -= 128;
	if (glyph.top >= 64) glyph.top -= 128;
	glyph.flags = (int)GetBits(bit_pos, 6); bit_pos += 6;
	int shadow_flags = (int)GetBits(bit_pos, 2) << 5; bit_pos += 2;
	shadow_flags |= (int)GetBits(bit_pos, 2) << 3; bit_pos += 2;
	shadow_flags |= (int)GetBits(bit_pos, 3); bit_pos += 3;
	glyph.shadow_id = (int)GetBits(bit_pos, 9); bit_pos += 9;
	(void)shadow_flags;
}

std::optional<FontGlyph> PgfFont::ReadChar(size_t bit_pos) const {
	if (bit_pos + 1024 > font_data_.size() * 8) {
		return std::nullopt;
	}
	FontGlyph glyph;
	SkipGlyphHeader(bit_pos, glyph);
	const int flags = glyph.flags;
	if (flags & 0x04) {
		const int index = (int)GetBits(bit_pos, 8); bit_pos += 8;
		if (index < dim_table_length_) {
			glyph.dimension_width = dimension_table_[0][index];
			glyph.dimension_height = dimension_table_[1][index];
		}
	} else {
		glyph.dimension_width = (int)GetBits(bit_pos, 32); bit_pos += 32;
		glyph.dimension_height = (int)GetBits(bit_pos, 32); bit_pos += 32;
	}
	if (flags & 0x08) {
		const int index = (int)GetBits(bit_pos, 8); bit_pos += 8;
		if (index < x_adjust_table_length_) {
			glyph.x_adjust_h = x_adjust_table_[0][index];
			glyph.x_adjust_v = x_adjust_table_[1][index];
		}
	} else {
		glyph.x_adjust_h = (int)GetBits(bit_pos, 32); bit_pos += 32;
		glyph.x_adjust_v = (int)GetBits(bit_pos, 32); bit_pos += 32;
	}
	if (flags & 0x10) {
		const int index = (int)GetBits(bit_pos, 8); bit_pos += 8;
		if (index < y_adjust_table_length_) {
			glyph.y_adjust_h = y_adjust_table_[0][index];
			glyph.y_adjust_v = y_adjust_table_[1][index];
		}
	} else {
		glyph.y_adjust_h = (int)GetBits(bit_pos, 32); bit_pos += 32;
		glyph.y_adjust_v = (int)GetBits(bit_pos, 32); bit_pos += 32;
	}
	if (flags & 0x20) {
		const int index = (int)GetBits(bit_pos, 8); bit_pos += 8;
		if (index < advance_table_length_) {
			glyph.advance_h = advance_table_[0][index];
			glyph.advance_v = advance_table_[1][index];
		}
	} else {
		glyph.advance_h = (int)GetBits(bit_pos, 32); bit_pos += 32;
		glyph.advance_v = (int)GetBits(bit_pos, 32); bit_pos += 32;
	}
	glyph.pixels = DecodeBitmap(bit_pos / 8 * 8, glyph.width, glyph.height);
	if ((glyph.flags & 0x03) == 0x02 && glyph.width > 0 && glyph.height > 0) {
		std::vector<uint8_t> transposed(glyph.pixels.size());
		for (int y = 0; y < glyph.height; ++y) {
			for (int x = 0; x < glyph.width; ++x) {
				transposed[y * glyph.width + x] = glyph.pixels[x * glyph.height + y];
			}
		}
		glyph.pixels = std::move(transposed);
	}
	return glyph;
}

std::optional<FontGlyph> PgfFont::ReadShadow(size_t bit_pos) const {
	auto glyph = ReadChar(bit_pos);
	if (!glyph.has_value()) {
		return std::nullopt;
	}
	const size_t skip = GetBits(bit_pos, 14);
	bit_pos += skip * 8;
	if (bit_pos + 96 > font_data_.size() * 8) {
		return std::nullopt;
	}
	FontGlyph shadow = *glyph;
	// The shadow sub-glyph stores only its size/bearing before the bitmap; unlike
	// the main glyph it has no flag/metric block. The pointer must therefore be
	// taken immediately after these four 7-bit fields.
	bit_pos += 14;
	shadow.width = (int)GetBits(bit_pos, 7); bit_pos += 7;
	shadow.height = (int)GetBits(bit_pos, 7); bit_pos += 7;
	shadow.left = (int)GetBits(bit_pos, 7); bit_pos += 7;
	shadow.top = (int)GetBits(bit_pos, 7); bit_pos += 7;
	if (shadow.left >= 64) shadow.left -= 128;
	if (shadow.top >= 64) shadow.top -= 128;
	shadow.pixels = DecodeBitmap(bit_pos / 8 * 8, shadow.width, shadow.height);
	if ((shadow.flags & 0x03) == 0x02 && shadow.width > 0 && shadow.height > 0) {
		std::vector<uint8_t> transposed(shadow.pixels.size());
		for (int y = 0; y < shadow.height; ++y) {
			for (int x = 0; x < shadow.width; ++x) {
				transposed[y * shadow.width + x] = shadow.pixels[x * shadow.height + y];
			}
		}
		shadow.pixels = std::move(transposed);
	}
	return shadow;
}

std::vector<uint8_t> PgfFont::DecodeBitmap(size_t bit_pos, int width, int height) const {
	const size_t total = (size_t)width * height;
	std::vector<uint8_t> pixels;
	if (total == 0) {
		return pixels;
	}
	pixels.reserve(total);
	const size_t limit = font_data_.size() * 8;
	while (pixels.size() < total && bit_pos + 8 < limit) {
		const int nibble = (int)GetBits(bit_pos, 4); bit_pos += 4;
		int value = 0;
		int run = 0;
		if (nibble < 8) {
			value = (int)GetBits(bit_pos, 4); bit_pos += 4;
			run = nibble + 1;
		} else {
			run = 16 - nibble;
		}
		for (int i = 0; i < run && pixels.size() < total; ++i) {
			if (nibble >= 8) {
				value = (int)GetBits(bit_pos, 4); bit_pos += 4;
			}
			pixels.push_back((uint8_t)value);
		}
	}
	if (pixels.size() < total) {
		return {};
	}
	return pixels;
}

uint32_t PgfFont::GlyphIndex(uint32_t codepoint) const {
	if (codepoint < first_glyph_) {
		return UINT32_MAX;
	}
	const size_t index = codepoint - first_glyph_;
	if (index >= charmap_.size()) {
		return UINT32_MAX;
	}
	const int mapped = charmap_[index];
	if (mapped < 0 || mapped >= (int)glyphs_.size()) {
		return UINT32_MAX;
	}
	return (uint32_t)mapped;
}

const FontGlyph *PgfFont::Glyph(uint32_t codepoint, bool shadow) const {
	const uint32_t index = GlyphIndex(codepoint);
	if (index == UINT32_MAX) {
		return nullptr;
	}
	const auto &entry = shadow ? shadow_glyphs_[index] : glyphs_[index];
	return entry.has_value() ? &entry.value() : nullptr;
}

float PgfFont::TextWidth(const std::string &text, float scale) const {
	size_t position = 0;
	float width = 0.0f;
	while (true) {
		const uint32_t codepoint = NextCodepoint(text, position);
		if (codepoint == 0) {
			break;
		}
		const FontGlyph *glyph = Glyph(codepoint);
		if (glyph) {
			width += glyph->advance_h / 64.0f * scale;
		}
	}
	return width;
}

}  // namespace xmb

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xmb {

// Decoded PGF glyph. The bitmap is the firmware's 4-bit alpha, stored one byte
// per pixel in row-major order (0..15).
struct FontGlyph {
	int width = 0;
	int height = 0;
	int left = 0;
	int top = 0;
	int flags = 0;
	int shadow_id = 0;
	int advance_h = 0;
	int advance_v = 0;
	int dimension_width = 0;
	int dimension_height = 0;
	int x_adjust_h = 0;
	int x_adjust_v = 0;
	int y_adjust_h = 0;
	int y_adjust_v = 0;
	std::vector<uint8_t> pixels;
};

// Reader for the firmware's PGF fonts (flash0:/font/ltn*.pgf).
class PgfFont {
public:
	bool Load(const std::string &path);

	const FontGlyph *Glyph(uint32_t codepoint, bool shadow = false) const;
	float TextWidth(const std::string &utf8, float scale) const;
	const std::string &name() const { return name_; }

private:
	uint32_t GetBits(size_t bit_pos, int count) const;
	void SkipGlyphHeader(size_t &bit_pos, FontGlyph &glyph) const;
	std::optional<FontGlyph> ReadChar(size_t bit_pos) const;
	std::optional<FontGlyph> ReadShadow(size_t bit_pos) const;
	std::vector<uint8_t> DecodeBitmap(size_t bit_pos, int width, int height) const;
	uint32_t GlyphIndex(uint32_t codepoint) const;

	std::vector<uint8_t> font_data_;
	std::vector<int> charmap_;
	std::vector<int> char_pointers_;
	std::vector<int> shadow_map_;
	std::vector<int> dimension_table_[2];
	std::vector<int> x_adjust_table_[2];
	std::vector<int> y_adjust_table_[2];
	std::vector<int> advance_table_[2];
	std::vector<std::optional<FontGlyph>> glyphs_;
	std::vector<std::optional<FontGlyph>> shadow_glyphs_;
	uint32_t first_glyph_ = 0;
	int dim_table_length_ = 0;
	int x_adjust_table_length_ = 0;
	int y_adjust_table_length_ = 0;
	int advance_table_length_ = 0;
	std::string name_;
};

// Decodes one UTF-8 codepoint; advances position. Returns 0 at the end.
uint32_t NextCodepoint(const std::string &text, size_t &position);

}  // namespace xmb

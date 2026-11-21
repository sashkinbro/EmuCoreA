#include "png_image.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

namespace xmb {
namespace {

constexpr uint32_t MakeTag(char a, char b, char c, char d) {
	return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16) |
	       ((uint32_t)(uint8_t)d << 24);
}

uint32_t ReadBe32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int Paeth(int a, int b, int c) {
	const int p = a + b - c;
	const int pa = p > a ? p - a : a - p;
	const int pb = p > b ? p - b : b - p;
	const int pc = p > c ? p - c : c - p;
	if (pa <= pb && pa <= pc) {
		return a;
	}
	return pb <= pc ? b : c;
}

bool InflateZlib(const std::vector<uint8_t> &compressed, size_t expected_size, std::vector<uint8_t> *out) {
	out->assign(expected_size, 0);
	if (expected_size == 0) {
		return true;
	}
	z_stream zs{};
	if (inflateInit(&zs) != Z_OK) {
		return false;
	}
	zs.next_in = const_cast<Bytef *>(compressed.data());
	zs.avail_in = (uInt)compressed.size();
	zs.next_out = out->data();
	zs.avail_out = (uInt)expected_size;
	const int res = inflate(&zs, Z_FINISH);
	const size_t total_out = zs.total_out;
	inflateEnd(&zs);
	return (res == Z_STREAM_END || res == Z_OK) && total_out == expected_size;
}

bool UnfilterScanlines(const uint8_t *src, size_t row_bytes, uint32_t pass_h, int bpp,
                       std::vector<uint8_t> *out_pixels) {
	out_pixels->assign(row_bytes * pass_h, 0);
	uint8_t *previous = nullptr;
	for (uint32_t y = 0; y < pass_h; ++y) {
		const uint8_t filter = *src++;
		uint8_t *row = out_pixels->data() + row_bytes * y;
		for (size_t x = 0; x < row_bytes; ++x) {
			const int a = x >= (size_t)bpp ? row[x - bpp] : 0;
			const int b = previous ? previous[x] : 0;
			const int c = (previous && x >= (size_t)bpp) ? previous[x - bpp] : 0;
			int value = src[x];
			switch (filter) {
			case 0: break;
			case 1: value += a; break;
			case 2: value += b; break;
			case 3: value += (a + b) >> 1; break;
			case 4: value += Paeth(a, b, c); break;
			default: return false;
			}
			row[x] = (uint8_t)value;
		}
		src += row_bytes;
		previous = row;
	}
	return true;
}

uint32_t ExtractSample(const uint8_t *row, size_t sample_index, int bit_depth) {
	if (bit_depth == 8) {
		return row[sample_index];
	}
	if (bit_depth == 16) {
		return ((uint32_t)row[sample_index * 2] << 8) | row[sample_index * 2 + 1];
	}
	const int samples_per_byte = 8 / bit_depth;
	const size_t byte_index = sample_index / samples_per_byte;
	const int in_byte = (int)(sample_index % samples_per_byte);
	const int shift = (8 - bit_depth) - in_byte * bit_depth;
	const uint32_t mask = (1u << bit_depth) - 1u;
	return (row[byte_index] >> shift) & mask;
}

uint8_t ScaleToU8(uint32_t sample, int bit_depth) {
	if (bit_depth == 8) {
		return (uint8_t)sample;
	}
	if (bit_depth == 16) {
		return (uint8_t)(sample >> 8);
	}
	const uint32_t max_val = (1u << bit_depth) - 1u;
	return (uint8_t)((sample * 255u + max_val / 2u) / max_val);
}

}  // namespace

bool ReadFile(const std::string &path, std::vector<uint8_t> *data) {
	std::ifstream stream(path, std::ios::binary);
	if (!stream) {
		return false;
	}
	stream.seekg(0, std::ios::end);
	const std::streamoff size = stream.tellg();
	if (size < 0 || size > 64 * 1024 * 1024) {
		return false;
	}
	stream.seekg(0, std::ios::beg);
	data->resize((size_t)size);
	stream.read(reinterpret_cast<char *>(data->data()), size);
	return stream.good() || stream.eof();
}

bool DecodePng(const std::vector<uint8_t> &file, int *width, int *height,
               std::vector<uint8_t> *rgba) {
	static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	if (file.size() < 8 || memcmp(file.data(), kSignature, sizeof(kSignature)) != 0) {
		return false;
	}

	uint32_t w = 0;
	uint32_t h = 0;
	int bit_depth = 0;
	int color_type = -1;
	int interlace = 0;
	std::vector<uint8_t> idat;
	std::vector<uint8_t> palette;
	std::vector<uint8_t> trns;
	size_t pos = 8;
	while (pos + 12 <= file.size()) {
		const uint32_t length = ReadBe32(&file[pos]);
		if (length > file.size() - pos - 12) {
			return false;
		}
		const uint32_t tag = MakeTag((char)file[pos + 4], (char)file[pos + 5], (char)file[pos + 6], (char)file[pos + 7]);
		const uint8_t *payload = &file[pos + 8];
		if (tag == MakeTag('I', 'H', 'D', 'R')) {
			if (length != 13) {
				return false;
			}
			w = ReadBe32(payload);
			h = ReadBe32(payload + 4);
			bit_depth = payload[8];
			color_type = payload[9];
			const uint8_t compression = payload[10];
			const uint8_t filter = payload[11];
			interlace = payload[12];
			const bool valid_depth =
			    (color_type == 0 && (bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8 || bit_depth == 16)) ||
			    (color_type == 3 && (bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8)) ||
			    ((color_type == 2 || color_type == 4 || color_type == 6) && (bit_depth == 8 || bit_depth == 16));
			if (!valid_depth || compression != 0 || filter != 0 || (interlace != 0 && interlace != 1)) {
				return false;
			}
		} else if (tag == MakeTag('P', 'L', 'T', 'E')) {
			if (length > 768 || length % 3 != 0) {
				return false;
			}
			palette.assign(payload, payload + length);
		} else if (tag == MakeTag('t', 'R', 'N', 'S')) {
			trns.assign(payload, payload + length);
		} else if (tag == MakeTag('I', 'D', 'A', 'T')) {
			idat.insert(idat.end(), payload, payload + length);
		} else if (tag == MakeTag('I', 'E', 'N', 'D')) {
			break;
		}
		pos += 12 + length;
	}
	if (w == 0 || h == 0 || w > 4096 || h > 4096 || idat.empty()) {
		return false;
	}
	if (color_type == 3 && palette.empty()) {
		return false;
	}

	const int samples = color_type == 6 ? 4 : (color_type == 4 ? 2 : (color_type == 2 ? 3 : 1));
	const int bpp = std::max(1, (samples * bit_depth + 7) / 8);

	static constexpr uint32_t kStartX[7] = {0, 4, 0, 2, 0, 1, 0};
	static constexpr uint32_t kStepX[7] = {8, 8, 4, 4, 2, 2, 1};
	static constexpr uint32_t kStartY[7] = {0, 0, 4, 0, 2, 0, 1};
	static constexpr uint32_t kStepY[7] = {8, 8, 8, 4, 4, 2, 2};

	const int num_passes = interlace == 1 ? 7 : 1;
	size_t total_raw_size = 0;
	for (int p = 0; p < num_passes; ++p) {
		const uint32_t sx = interlace == 1 ? kStartX[p] : 0;
		const uint32_t dx = interlace == 1 ? kStepX[p] : 1;
		const uint32_t sy = interlace == 1 ? kStartY[p] : 0;
		const uint32_t dy = interlace == 1 ? kStepY[p] : 1;
		if (w <= sx || h <= sy) {
			continue;
		}
		const uint32_t pw = (w - 1 - sx) / dx + 1;
		const uint32_t ph = (h - 1 - sy) / dy + 1;
		const size_t row_bytes = ((size_t)pw * samples * bit_depth + 7) / 8;
		total_raw_size += (row_bytes + 1) * ph;
	}

	std::vector<uint8_t> raw;
	if (!InflateZlib(idat, total_raw_size, &raw)) {
		return false;
	}

	const size_t output_stride = (size_t)w * 4;
	std::vector<uint8_t> pass_pixels;
	if (interlace == 0 && bit_depth == 8) {
		if (color_type == 6) {
			if (!UnfilterScanlines(raw.data(), output_stride, h, 4, rgba)) {
				return false;
			}
			*width = (int)w;
			*height = (int)h;
			return true;
		}
		if (color_type == 2 && trns.empty()) {
			const size_t row_bytes = (size_t)w * 3;
			if (!UnfilterScanlines(raw.data(), row_bytes, h, 3, &pass_pixels)) {
				return false;
			}
			rgba->resize(output_stride * h);
			const uint8_t *src = pass_pixels.data();
			uint8_t *dst = rgba->data();
			const size_t pixel_count = (size_t)w * h;
			for (size_t i = 0; i < pixel_count; ++i) {
				dst[0] = src[0];
				dst[1] = src[1];
				dst[2] = src[2];
				dst[3] = 255;
				src += 3;
				dst += 4;
			}
			*width = (int)w;
			*height = (int)h;
			return true;
		}
	}

	std::vector<uint8_t> pixels(output_stride * h, 0);
	size_t raw_offset = 0;
	for (int p = 0; p < num_passes; ++p) {
		const uint32_t sx = interlace == 1 ? kStartX[p] : 0;
		const uint32_t dx = interlace == 1 ? kStepX[p] : 1;
		const uint32_t sy = interlace == 1 ? kStartY[p] : 0;
		const uint32_t dy = interlace == 1 ? kStepY[p] : 1;
		if (w <= sx || h <= sy) {
			continue;
		}
		const uint32_t pw = (w - 1 - sx) / dx + 1;
		const uint32_t ph = (h - 1 - sy) / dy + 1;
		const size_t row_bytes = ((size_t)pw * samples * bit_depth + 7) / 8;
		if (!UnfilterScanlines(raw.data() + raw_offset, row_bytes, ph, bpp, &pass_pixels)) {
			return false;
		}
		raw_offset += (row_bytes + 1) * ph;

		for (uint32_t py = 0; py < ph; ++py) {
			const uint32_t dst_y = sy + py * dy;
			const uint8_t *row = pass_pixels.data() + row_bytes * py;
			uint8_t *out_row = pixels.data() + output_stride * dst_y;
			for (uint32_t px = 0; px < pw; ++px) {
				const uint32_t dst_x = sx + px * dx;
				uint8_t r = 0, g = 0, b = 0, a = 255;
				const size_t s_base = (size_t)px * samples;
				if (color_type == 6) {
					r = ScaleToU8(ExtractSample(row, s_base + 0, bit_depth), bit_depth);
					g = ScaleToU8(ExtractSample(row, s_base + 1, bit_depth), bit_depth);
					b = ScaleToU8(ExtractSample(row, s_base + 2, bit_depth), bit_depth);
					a = ScaleToU8(ExtractSample(row, s_base + 3, bit_depth), bit_depth);
				} else if (color_type == 4) {
					r = g = b = ScaleToU8(ExtractSample(row, s_base + 0, bit_depth), bit_depth);
					a = ScaleToU8(ExtractSample(row, s_base + 1, bit_depth), bit_depth);
				} else if (color_type == 2) {
					const uint32_t sr = ExtractSample(row, s_base + 0, bit_depth);
					const uint32_t sg = ExtractSample(row, s_base + 1, bit_depth);
					const uint32_t sb = ExtractSample(row, s_base + 2, bit_depth);
					r = ScaleToU8(sr, bit_depth);
					g = ScaleToU8(sg, bit_depth);
					b = ScaleToU8(sb, bit_depth);
					if (trns.size() >= 6) {
						const uint32_t tr = ((uint32_t)trns[0] << 8) | trns[1];
						const uint32_t tg = ((uint32_t)trns[2] << 8) | trns[3];
						const uint32_t tb = ((uint32_t)trns[4] << 8) | trns[5];
						if (sr == tr && sg == tg && sb == tb) {
							a = 0;
						}
					}
				} else if (color_type == 0) {
					const uint32_t sg = ExtractSample(row, s_base, bit_depth);
					r = g = b = ScaleToU8(sg, bit_depth);
					if (trns.size() >= 2) {
						const uint32_t tg = ((uint32_t)trns[0] << 8) | trns[1];
						if (sg == tg) {
							a = 0;
						}
					}
				} else {
					const uint32_t index = ExtractSample(row, s_base, bit_depth);
					const size_t palette_index = (size_t)index * 3;
					if (palette_index + 2 >= palette.size()) {
						return false;
					}
					r = palette[palette_index + 0];
					g = palette[palette_index + 1];
					b = palette[palette_index + 2];
					if (index < trns.size()) {
						a = trns[index];
					}
				}
				uint8_t *out = out_row + dst_x * 4;
				out[0] = r;
				out[1] = g;
				out[2] = b;
				out[3] = a;
			}
		}
	}

	*width = (int)w;
	*height = (int)h;
	*rgba = std::move(pixels);
	return true;
}

}  // namespace xmb

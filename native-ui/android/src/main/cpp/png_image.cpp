#include "png_image.h"

#include <zlib.h>

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
	std::vector<uint8_t> idat;
	std::vector<uint8_t> palette;
	std::vector<uint8_t> palette_alpha;
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
			const uint8_t interlace = payload[12];
			if (bit_depth != 8 || (color_type != 0 && color_type != 2 && color_type != 3 && color_type != 6) ||
			    compression != 0 || filter != 0 || interlace != 0) {
				return false;
			}
		} else if (tag == MakeTag('P', 'L', 'T', 'E')) {
			if (length > 768 || length % 3 != 0) {
				return false;
			}
			palette.assign(payload, payload + length);
		} else if (tag == MakeTag('t', 'R', 'N', 'S')) {
			palette_alpha.assign(payload, payload + length);
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
	const int bpp = color_type == 6 ? 4 : (color_type == 2 ? 3 : 1);
	const size_t source_stride = (size_t)w * bpp;
	const size_t raw_size = (source_stride + 1) * h;
	std::vector<uint8_t> raw(raw_size);
	uLongf unpacked = (uLongf)raw_size;
	const int result = uncompress(raw.data(), &unpacked, idat.data(), (uLong)idat.size());
	if (result != Z_OK || unpacked != raw_size) {
		return false;
	}

	std::vector<uint8_t> source_pixels(source_stride * h);
	const uint8_t *source = raw.data();
	uint8_t *previous = nullptr;
	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t filter = *source++;
		uint8_t *row = source_pixels.data() + source_stride * y;
		for (size_t x = 0; x < source_stride; ++x) {
			const int a = x >= (size_t)bpp ? row[x - bpp] : 0;
			const int b = previous ? previous[x] : 0;
			const int c = (previous && x >= (size_t)bpp) ? previous[x - bpp] : 0;
			int value = source[x];
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
		source += source_stride;
		previous = row;
	}

	const size_t output_stride = (size_t)w * 4;
	std::vector<uint8_t> pixels(output_stride * h);
	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t *row = source_pixels.data() + source_stride * y;
		uint8_t *out = pixels.data() + output_stride * y;
		for (uint32_t x = 0; x < w; ++x) {
			uint8_t r = 0, g = 0, b = 0, a = 255;
			if (color_type == 6) {
				r = row[x * 4 + 0]; g = row[x * 4 + 1]; b = row[x * 4 + 2]; a = row[x * 4 + 3];
			} else if (color_type == 2) {
				r = row[x * 3 + 0]; g = row[x * 3 + 1]; b = row[x * 3 + 2];
			} else if (color_type == 0) {
				r = g = b = row[x];
			} else {
				const uint8_t index = row[x];
				const size_t palette_index = (size_t)index * 3;
				if (palette_index + 2 >= palette.size()) {
					return false;
				}
				r = palette[palette_index + 0];
				g = palette[palette_index + 1];
				b = palette[palette_index + 2];
				if (index < palette_alpha.size()) {
					a = palette_alpha[index];
				}
			}
			out[x * 4 + 0] = r;
			out[x * 4 + 1] = g;
			out[x * 4 + 2] = b;
			out[x * 4 + 3] = a;
		}
	}

	*width = (int)w;
	*height = (int)h;
	*rgba = std::move(pixels);
	return true;
}

}  // namespace xmb

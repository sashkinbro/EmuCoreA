// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

#include "psp_formats.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace xmb {

namespace {

// Probability block layout, in bytes from the start of the block (see
// native-ui/tools/psp/rlz.py and native-ui/tools/README.md for the details
// that make this decode the firmware streams: 18 distance slots, standard
// adaptive probabilities, raw bisection run normalised once).
constexpr size_t kLiterals = 0;
constexpr size_t kDistLong = 8 * 0xFF;
constexpr size_t kDistShort = kDistLong + 0xF0;
constexpr size_t kDistances = kDistShort + 0x40;
constexpr size_t kDistanceSlots = 18;
constexpr size_t kLenDesc = kDistances + kDistanceSlots * 0x20;
constexpr size_t kMatchLens = kLenDesc + 0x40;
constexpr size_t kProbBlockSize = kMatchLens + 0xFF;

class RangeDecoder {
public:
	RangeDecoder(const uint8_t *data, size_t size, uint8_t *probs) : data_(data), size_(size), probs_(probs) {
		if (size < 4)
			throw PspError("RLZ stream is shorter than the range coder initial code");
		for (int i = 0; i < 4; i++)
			code_ = (code_ << 8) | Byte();
	}

	void Normalise() {
		if (range_ < 0x01000000u) {
			range_ <<= 8;
			code_ = (code_ << 8) | Byte();
		}
	}

	int Bit(size_t index) {
		if (index >= kProbBlockSize)
			throw PspError("RLZ probability index outside the block");
		uint32_t probability = probs_[index];
		uint32_t midpoint = (range_ >> 8) * probability;
		int bit;
		if (code_ < midpoint) {
			bit = 1;
			range_ = midpoint;
			probability += (0xFF - probability) >> 3;
		} else {
			bit = 0;
			code_ -= midpoint;
			range_ -= midpoint;
			probability -= probability >> 3;
		}
		probs_[index] = (uint8_t)(probability & 0xFF);
		Normalise();
		return bit;
	}

	int UniformBit() {
		range_ >>= 1;
		if (code_ < range_)
			return 1;
		code_ -= range_;
		return 0;
	}

private:
	uint8_t Byte() {
		if (pos_ >= size_)
			throw PspError("RLZ stream ended inside a symbol");
		return data_[pos_++];
	}

	const uint8_t *data_;
	size_t size_;
	size_t pos_ = 0;
	uint8_t *probs_;
	uint32_t range_ = 0xFFFFFFFFu;
	uint32_t code_ = 0;
};

uint64_t Special(RangeDecoder &rd, size_t offset, int arg) {
	uint64_t value = 0;
	if (arg >= 3) {
		value |= (uint64_t)rd.Bit(offset) << arg;
		arg--;
		if (arg >= 3) {
			value |= (uint64_t)rd.Bit(offset) << arg;
			arg--;
		}
		offset++;
		if (arg >= 3) {
			for (int k = 0; k < arg - 2; k++)
				value |= (uint64_t)rd.UniformBit() << (arg - k);
			rd.Normalise();
			arg = 2;
		}
	}
	while (arg >= 0) {
		value |= (uint64_t)rd.Bit(offset) << arg;
		offset++;
		arg--;
	}
	return value;
}

}  // namespace

std::vector<uint8_t> RlzDecompress(const uint8_t *data, size_t size, size_t expected_size) {
	if (size == 0)
		throw PspError("empty RLZ stream");
	const int mode = data[0];
	const int real_mode = mode & 7;
	const uint8_t initial = (uint8_t)((4 - ((mode >> 3) & 3)) << 5);
	std::vector<uint8_t> probs(kProbBlockSize, initial);

	RangeDecoder rd(data + 1, size - 1, probs.data());
	std::vector<uint8_t> out(expected_size);
	size_t n = 0;
	unsigned last_byte = 0;
	unsigned var_c = 0;

	while (n < expected_size) {
		int ones = 0;
		for (int i = 0; i < 8; i++) {
			if (!rd.Bit(kLenDesc + i * 8 + var_c))
				break;
			ones++;
		}
		uint64_t length_code;
		int ret;
		if (ones == 0) {
			length_code = 0;
			ret = -1;
		} else if (ones == 8) {
			uint64_t low = Special(rd, kMatchLens + ((var_c & 7) + 6 * 8) * 4 + ((n << 6) & 3), 6);
			length_code = 128 + low;
			ret = 7;
		} else {
			int floor = ones - 1;
			uint64_t low = 0;
			if (floor != 0) {
				low = Special(rd, kMatchLens + ((var_c & 7) + (size_t)(floor - 1) * 8) * 4 + ((n << (floor - 1)) & 3),
				              floor - 1);
			}
			length_code = ((uint64_t)1 << floor) + low;
			ret = ones - 1;
		}

		if (length_code == 255)
			throw PspError("RLZ end marker before the declared uncompressed size");
		const uint64_t match_len = length_code + 1;

		if (match_len == 1) {
			unsigned context = ((((unsigned)(n & 7) << 8) | last_byte) >> real_mode) & 7;
			unsigned prefix = 0;
			for (int k = 0; k < 8; k++)
				prefix = (prefix << 1) | (unsigned)rd.Bit(kLiterals + context * 0xFF + ((1u << k) - 1) + prefix);
			uint8_t byte = (uint8_t)(prefix & 0xFF);
			out[n++] = byte;
			last_byte = byte;
			if (var_c > 0)
				var_c--;
			continue;
		}

		int bits;
		size_t base;
		if (match_len < 4) {
			bits = 3;
			base = kDistShort;
		} else {
			bits = 5;
			base = kDistLong;
		}
		unsigned tree = 1;
		for (int i = 0; i < bits; i++)
			tree = (tree << 1) | (unsigned)rd.Bit(base + (size_t)tree * 8 + (size_t)ret);
		int floor = (int)(tree - (1u << bits)) - (match_len < 4 ? 0 : 0xC);
		if (floor < 0)
			throw PspError("negative distance exponent " + std::to_string(floor) + " at output " + std::to_string(n));
		uint64_t distance = (uint64_t)1 << floor;
		if (floor > 0)
			distance |= Special(rd, kDistances + (size_t)(floor - 1) * 0x20, floor - 1);
		if (distance > n)
			throw PspError("distance " + std::to_string(distance) + " before output start at " + std::to_string(n));
		size_t source = n - (size_t)distance;
		for (uint64_t i = 0; i < match_len; i++) {
			if (n >= expected_size)
				throw PspError("RLZ stream expands past the declared size");
			out[n++] = out[source++];
		}
		last_byte = out[n - 1];
		var_c = 7 - (unsigned)(n & 1);
	}
	return out;
}

bool ZlibInflate(const uint8_t *data, size_t size, std::vector<uint8_t> *out) {
	out->clear();
	z_stream zs;
	std::memset(&zs, 0, sizeof(zs));
	if (inflateInit(&zs) != Z_OK)
		return false;
	zs.next_in = const_cast<Bytef *>(data);
	zs.avail_in = (uInt)std::min<size_t>(size, 0xFFFFFFFFu);
	out->resize(std::max<size_t>(size * 3, 4096));
	size_t produced = 0;
	int rc;
	for (;;) {
		zs.next_out = out->data() + produced;
		zs.avail_out = (uInt)(out->size() - produced);
		rc = inflate(&zs, Z_NO_FLUSH);
		produced = out->size() - zs.avail_out;
		if (rc == Z_STREAM_END)
			break;
		if ((rc == Z_OK || rc == Z_BUF_ERROR) && zs.avail_out == 0) {
			out->resize(out->size() * 2);
			continue;
		}
		break;  // error, or truncated input
	}
	inflateEnd(&zs);
	if (rc != Z_STREAM_END) {
		out->clear();
		return false;
	}
	out->resize(produced);
	return true;
}

}  // namespace xmb

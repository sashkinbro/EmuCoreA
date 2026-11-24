// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

#include "psp_formats.h"

#include <algorithm>
#include <cstring>

// Port of native-ui/tools/psp/vag.py.

namespace xmb {

namespace {

const int kCoefficients[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

uint32_t Be32(const uint8_t *d, size_t off) {
	return ((uint32_t)d[off] << 24) | ((uint32_t)d[off + 1] << 16) | ((uint32_t)d[off + 2] << 8) | d[off + 3];
}

void PutLe32(std::vector<uint8_t> *v, uint32_t x) {
	for (int i = 0; i < 4; i++)
		v->push_back((uint8_t)(x >> (8 * i)));
}

void PutLe16(std::vector<uint8_t> *v, uint32_t x) {
	v->push_back((uint8_t)x);
	v->push_back((uint8_t)(x >> 8));
}

}  // namespace

VagSound VagDecode(const uint8_t *data, size_t size) {
	if (size < 0x30 || memcmp(data, "VAGp", 4) != 0)
		throw PspError("not a VAG file");
	uint32_t version = Be32(data, 4);
	if (version != 4 && version != 0x20)
		throw PspError("unexpected VAG version " + std::to_string(version));
	const uint32_t payload_size = Be32(data, 0x0C);
	VagSound out;
	out.sample_rate = Be32(data, 0x10);
	if (out.sample_rate == 0)
		throw PspError("VAG has no sample rate");
	const size_t start = 0x30;
	const uint64_t end = (uint64_t)start + payload_size;
	if (end > size || payload_size % 16)
		throw PspError("VAG payload size is invalid");

	out.samples.reserve((size_t)payload_size / 16 * 28);
	int previous1 = 0, previous2 = 0;
	for (size_t frame = start; frame < end; frame += 16) {
		const uint8_t header = data[frame];
		const int shift = header & 0x0F;
		if (shift > 12)
			throw PspError("VAG shift " + std::to_string(shift) + " is invalid");
		const int filter_index = (header >> 4) & 0x07;
		if (filter_index >= 5)
			throw PspError("VAG filter " + std::to_string(filter_index) + " is invalid");
		const int c1 = kCoefficients[filter_index][0];
		const int c2 = kCoefficients[filter_index][1];
		for (int i = 2; i < 16; i++) {
			const uint8_t byte = data[frame + i];
			for (int half = 0; half < 2; half++) {
				int nibble = half == 0 ? (byte & 0x0F) : (byte >> 4);
				int value = (nibble & 0x08) ? nibble - 16 : nibble;
				int sample = (value * 4096) >> shift;
				sample += (previous1 * c1 + previous2 * c2) >> 6;
				sample = std::max(-32768, std::min(32767, sample));
				previous2 = previous1;
				previous1 = sample;
				out.samples.push_back((int16_t)sample);
			}
		}
	}
	return out;
}

std::vector<uint8_t> WavBuild(uint32_t sample_rate, int channels, const std::vector<int16_t> &samples) {
	const uint32_t block_align = 2 * channels;
	const uint32_t byte_rate = sample_rate * block_align;
	const uint32_t data_len = (uint32_t)(samples.size() * 2);
	std::vector<uint8_t> out;
	out.reserve(44 + data_len);
	out.insert(out.end(), {'R', 'I', 'F', 'F'});
	PutLe32(&out, 36 + data_len);
	out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
	PutLe32(&out, 16);
	PutLe16(&out, 1);
	PutLe16(&out, (uint32_t)channels);
	PutLe32(&out, sample_rate);
	PutLe32(&out, byte_rate);
	PutLe16(&out, block_align);
	PutLe16(&out, 16);
	out.insert(out.end(), {'d', 'a', 't', 'a'});
	PutLe32(&out, data_len);
	for (int16_t s : samples)
		PutLe16(&out, (uint16_t)s);
	return out;
}

}  // namespace xmb

// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Decoders for the PSP container formats found in flash0:/vsh/resource. These
// are straight ports of native-ui/tools/psp/{rlz,gim,vag}.py; behaviour and
// error conditions follow the Python reference so both tools agree.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace xmb {

struct PspError : std::runtime_error {
	explicit PspError(const std::string &message) : std::runtime_error(message) {}
};

// RLZ range-coded stream (data starts with the mode byte). Throws PspError if
// the stream is malformed or does not decode to exactly expected_size bytes.
std::vector<uint8_t> RlzDecompress(const uint8_t *data, size_t size, size_t expected_size);

// zlib stream helper. Returns false when the stream is invalid or incomplete.
// Trailing bytes after the end of the stream are ignored, like Python's
// zlib.decompress.
bool ZlibInflate(const uint8_t *data, size_t size, std::vector<uint8_t> *out);

struct GimImage {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgba;  // width * height * 4
};

// Decodes the first picture of a little-endian PSP GIM. Throws PspError.
GimImage GimDecode(const uint8_t *data, size_t size);

// 8-bit RGBA, non-interlaced PNG.
std::vector<uint8_t> PngEncode(int width, int height, const uint8_t *rgba);

struct VagSound {
	uint32_t sample_rate = 0;
	std::vector<int16_t> samples;
};

VagSound VagDecode(const uint8_t *data, size_t size);

// PCM16 WAV with the given interleaved samples.
std::vector<uint8_t> WavBuild(uint32_t sample_rate, int channels, const std::vector<int16_t> &samples);

}  // namespace xmb

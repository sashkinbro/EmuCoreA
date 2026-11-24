// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xmb {

/**
 * Decodes a PSP SND0.AT3 file (containing ATRAC3 or ATRAC3plus audio in a RIFF container)
 * into standard 16-bit stereo PCM WAV bytes or writes to a .wav file.
 */
bool DecodeAt3ToWav(const uint8_t *at3_data, size_t at3_size, std::vector<uint8_t> &out_wav);
bool DecodeAt3FileToWavFile(const std::string &at3_path, const std::string &wav_path);

}  // namespace xmb

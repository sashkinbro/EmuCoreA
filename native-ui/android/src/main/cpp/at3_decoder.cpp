// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

#include "at3_decoder.h"

#include <android/log.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

#include "at3_decoders.h"

namespace xmb {
namespace {

constexpr const char *kTag = "EmuCoreAt3";

uint32_t ReadU32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t ReadU16(const uint8_t *p) {
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void WriteU32(std::vector<uint8_t> &out, uint32_t val) {
	out.push_back((uint8_t)(val & 0xFF));
	out.push_back((uint8_t)((val >> 8) & 0xFF));
	out.push_back((uint8_t)((val >> 16) & 0xFF));
	out.push_back((uint8_t)((val >> 24) & 0xFF));
}

void WriteU16(std::vector<uint8_t> &out, uint16_t val) {
	out.push_back((uint8_t)(val & 0xFF));
	out.push_back((uint8_t)((val >> 8) & 0xFF));
}

void WriteFourCC(std::vector<uint8_t> &out, const char cc[4]) {
	out.push_back((uint8_t)cc[0]);
	out.push_back((uint8_t)cc[1]);
	out.push_back((uint8_t)cc[2]);
	out.push_back((uint8_t)cc[3]);
}

}  // namespace

bool DecodeAt3ToWav(const uint8_t *at3_data, size_t at3_size, std::vector<uint8_t> &out_wav) {
	out_wav.clear();
	if (at3_data == nullptr || at3_size < 44) {
		__android_log_print(ANDROID_LOG_ERROR, kTag, "AT3 data too short: %zu", at3_size);
		return false;
	}
	if (std::memcmp(at3_data, "RIFF", 4) != 0 || std::memcmp(at3_data + 8, "WAVE", 4) != 0) {
		__android_log_print(ANDROID_LOG_ERROR, kTag, "Not a valid RIFF/WAVE file");
		return false;
	}

	const uint8_t *fmt_data = nullptr;
	uint32_t fmt_size = 0;
	const uint8_t *raw_audio = nullptr;
	uint32_t raw_audio_size = 0;
	uint32_t skip_samples = 0;
	uint32_t fact_total_samples = 0;

	size_t pos = 12;
	while (pos + 8 <= at3_size) {
		const uint8_t *chunk_hdr = at3_data + pos;
		uint32_t chunk_size = ReadU32(chunk_hdr + 4);
		pos += 8;
		if (pos > at3_size) {
			break;
		}
		uint32_t avail = (uint32_t)std::min<size_t>(chunk_size, at3_size - pos);
		if (std::memcmp(chunk_hdr, "fmt ", 4) == 0) {
			fmt_data = at3_data + pos;
			fmt_size = avail;
		} else if (std::memcmp(chunk_hdr, "fact", 4) == 0) {
			if (avail >= 4) {
				fact_total_samples = ReadU32(at3_data + pos);
			}
			if (avail >= 12) {
				// Sony AA3/OMA-in-RIFF uses offset +8 for skip samples (offset +4 in shorter facts).
				uint32_t s8 = ReadU32(at3_data + pos + 8);
				uint32_t s4 = ReadU32(at3_data + pos + 4);
				skip_samples = s8 <= 8192 ? s8 : (s4 <= 8192 ? s4 : 0);
			} else if (avail >= 8) {
				uint32_t s4 = ReadU32(at3_data + pos + 4);
				skip_samples = s4 <= 8192 ? s4 : 0;
			}
		} else if (std::memcmp(chunk_hdr, "data", 4) == 0) {
			raw_audio = at3_data + pos;
			raw_audio_size = avail;
		}
		if (chunk_size > avail) {
			break;
		}
		pos += (chunk_size + 1) & ~1;  // RIFF chunks are word-aligned
	}

	if (fmt_data == nullptr || fmt_size < 16 || raw_audio == nullptr || raw_audio_size == 0) {
		__android_log_print(ANDROID_LOG_ERROR, kTag, "Missing fmt or data chunk in AT3");
		return false;
	}

	uint16_t format_tag = ReadU16(fmt_data);
	uint16_t channels = ReadU16(fmt_data + 2);
	uint32_t sample_rate = ReadU32(fmt_data + 4);
	uint16_t block_align = ReadU16(fmt_data + 12);

	if (channels == 0 || block_align == 0 || sample_rate == 0) {
		__android_log_print(ANDROID_LOG_ERROR, kTag, "Invalid format params: ch=%u, rate=%u, ba=%u",
		                    channels, sample_rate, block_align);
		return false;
	}

	// Distinguish ATRAC3 vs ATRAC3+ when wrapped in WAVE_FORMAT_EXTENSIBLE (0xFFFE).
	bool is_atrac3 = (format_tag == 0x0270);
	bool is_atrac3plus = (format_tag == 0xFFFE);
	if (format_tag == 0xFFFE && fmt_size >= 40) {
		static const uint8_t kAtr3GuidHead[4] = {0x70, 0x02, 0x00, 0x00};
		if (std::memcmp(fmt_data + 24, kAtr3GuidHead, 4) == 0) {
			is_atrac3 = true;
			is_atrac3plus = false;
		}
	}

	std::vector<int16_t> pcm_samples;
	float left_buf[2048]{};
	float right_buf[2048]{};
	float *out_planes[2] = {left_buf, right_buf};

	if (is_atrac3plus) {
		int ba = (int)block_align;
		ATRAC3PContext *ctx = atrac3p_alloc(channels, &ba);
		if (ctx == nullptr) {
			__android_log_print(ANDROID_LOG_ERROR, kTag, "atrac3p_alloc failed");
			return false;
		}
		size_t in_offset = 0;
		while (in_offset + block_align <= raw_audio_size) {
			int nb_samples = 0;
			int res = atrac3p_decode_frame(ctx, out_planes, &nb_samples, raw_audio + in_offset, block_align);
			in_offset += block_align;
			if (res < 0 || nb_samples <= 0) {
				continue;
			}
			for (int i = 0; i < nb_samples; ++i) {
				float l = left_buf[i];
				float r = channels > 1 ? right_buf[i] : left_buf[i];
				int16_t s_l = (int16_t)std::clamp(l * 32767.0f, -32768.0f, 32767.0f);
				int16_t s_r = (int16_t)std::clamp(r * 32767.0f, -32768.0f, 32767.0f);
				pcm_samples.push_back(s_l);
				pcm_samples.push_back(s_r);
			}
		}
		atrac3p_free(ctx);
	} else if (is_atrac3) {
		const uint8_t *extra = nullptr;
		int extra_size = 0;
		if (fmt_size >= 32) {
			extra = fmt_data + 18;
			extra_size = std::min<int>(14, (int)fmt_size - 18);
		}
		int ba = (int)block_align;
		ATRAC3Context *ctx = atrac3_alloc(channels, &ba, extra, extra_size);
		if (ctx == nullptr) {
			__android_log_print(ANDROID_LOG_ERROR, kTag, "atrac3_alloc failed");
			return false;
		}
		size_t in_offset = 0;
		while (in_offset + block_align <= raw_audio_size) {
			int nb_samples = 0;
			int res = atrac3_decode_frame(ctx, out_planes, &nb_samples, raw_audio + in_offset, block_align);
			in_offset += block_align;
			if (res < 0 || nb_samples <= 0) {
				continue;
			}
			for (int i = 0; i < nb_samples; ++i) {
				float l = left_buf[i];
				float r = channels > 1 ? right_buf[i] : left_buf[i];
				int16_t s_l = (int16_t)std::clamp(l * 32767.0f, -32768.0f, 32767.0f);
				int16_t s_r = (int16_t)std::clamp(r * 32767.0f, -32768.0f, 32767.0f);
				pcm_samples.push_back(s_l);
				pcm_samples.push_back(s_r);
			}
		}
		atrac3_free(ctx);
	} else {
		__android_log_print(ANDROID_LOG_ERROR, kTag, "Unsupported format tag: 0x%04x", format_tag);
		return false;
	}

	if (pcm_samples.empty()) {
		__android_log_print(ANDROID_LOG_ERROR, kTag, "No PCM samples decoded from AT3");
		return false;
	}

	// Trim leading encoder delay / padding and trailing samples per fact chunk for clean looping.
	const size_t total_frames = pcm_samples.size() / 2;
	const size_t trim_start = (skip_samples > 0 && skip_samples < total_frames / 2) ? skip_samples : 0;
	size_t keep_frames = total_frames - trim_start;
	if (fact_total_samples > 0 && fact_total_samples < keep_frames) {
		keep_frames = fact_total_samples;
	}
	const size_t start_idx = trim_start * 2;
	const size_t keep_samples = keep_frames * 2;

	// Write standard 16-bit stereo PCM WAV
	const uint32_t pcm_bytes = (uint32_t)(keep_samples * sizeof(int16_t));
	const uint32_t riff_payload = 36 + pcm_bytes;
	const uint16_t out_channels = 2;
	const uint32_t byte_rate = sample_rate * out_channels * sizeof(int16_t);
	const uint16_t block_align_out = out_channels * sizeof(int16_t);

	out_wav.reserve(44 + pcm_bytes);
	WriteFourCC(out_wav, "RIFF");
	WriteU32(out_wav, riff_payload);
	WriteFourCC(out_wav, "WAVE");

	WriteFourCC(out_wav, "fmt ");
	WriteU32(out_wav, 16);
	WriteU16(out_wav, 1);
	WriteU16(out_wav, out_channels);
	WriteU32(out_wav, sample_rate);
	WriteU32(out_wav, byte_rate);
	WriteU16(out_wav, block_align_out);
	WriteU16(out_wav, 16);

	WriteFourCC(out_wav, "data");
	WriteU32(out_wav, pcm_bytes);
	const uint8_t *sample_bytes = reinterpret_cast<const uint8_t *>(pcm_samples.data() + start_idx);
	out_wav.insert(out_wav.end(), sample_bytes, sample_bytes + pcm_bytes);

	__android_log_print(ANDROID_LOG_INFO, kTag, "Decoded AT3 to WAV: %zu frames (%u Hz stereo, %zu bytes)",
	                    keep_frames, sample_rate, out_wav.size());
	return true;
}

bool DecodeAt3FileToWavFile(const std::string &at3_path, const std::string &wav_path) {
	std::ifstream file(at3_path, std::ios::binary | std::ios::ate);
	if (!file.is_open()) {
		__android_log_print(ANDROID_LOG_ERROR, kTag, "Cannot open AT3 file: %s", at3_path.c_str());
		return false;
	}
	const std::streamsize size = file.tellg();
	if (size <= 0) {
		return false;
	}
	file.seekg(0, std::ios::beg);
	std::vector<uint8_t> buffer((size_t)size);
	if (!file.read(reinterpret_cast<char *>(buffer.data()), size)) {
		return false;
	}
	file.close();

	std::vector<uint8_t> wav_data;
	if (!DecodeAt3ToWav(buffer.data(), buffer.size(), wav_data)) {
		return false;
	}

	std::ofstream out(wav_path, std::ios::binary);
	if (!out.is_open()) {
		__android_log_print(ANDROID_LOG_ERROR, kTag, "Cannot create WAV file: %s", wav_path.c_str());
		return false;
	}
	out.write(reinterpret_cast<const char *>(wav_data.data()), (std::streamsize)wav_data.size());
	out.close();
	return true;
}

}  // namespace xmb

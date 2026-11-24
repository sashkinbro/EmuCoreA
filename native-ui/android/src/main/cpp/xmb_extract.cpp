// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

#include "xmb_extract.h"

#include "psp_formats.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <utility>

#ifndef XMB_EXTRACT_STANDALONE
#include <jni.h>
#endif

// C++ port of native-ui/tools/extract_psp_resources.py together with the RCO
// reader of native-ui/tools/psp/rco.py. The output tree, file names and
// manifest.json fields are the same; see that script for the intent.

namespace fs = std::filesystem;

namespace xmb {

namespace {

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------- helpers

std::string Hex6(uint64_t v) {
	char buf[24];
	snprintf(buf, sizeof(buf), "%06llx", (unsigned long long)v);
	return buf;
}

Bytes ReadAll(const fs::path &path) {
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (!in)
		throw PspError("cannot read " + path.filename().string());
	std::streamoff size = in.tellg();
	in.seekg(0);
	Bytes data((size_t)size);
	if (size > 0 && !in.read(reinterpret_cast<char *>(data.data()), size))
		throw PspError("cannot read " + path.filename().string());
	return data;
}

void WriteAll(const fs::path &path, const uint8_t *data, size_t size) {
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (out && size)
		out.write(reinterpret_cast<const char *>(data), (std::streamsize)size);
	if (!out)
		throw PspError("cannot write " + path.filename().string());
}

void WriteAll(const fs::path &path, const Bytes &data) { WriteAll(path, data.data(), data.size()); }

// SHA-256 -------------------------------------------------------------------

class Sha256 {
public:
	Sha256() { Reset(); }

	void Update(const uint8_t *p, size_t n) {
		total_ += n;
		while (n) {
			size_t take = std::min<size_t>(64 - fill_, n);
			memcpy(buf_ + fill_, p, take);
			fill_ += take;
			p += take;
			n -= take;
			if (fill_ == 64) {
				Block(buf_);
				fill_ = 0;
			}
		}
	}

	std::string Final() {
		uint64_t bits = total_ * 8;
		uint8_t pad = 0x80;
		Update(&pad, 1);
		uint8_t zero = 0;
		while (fill_ != 56)
			Update(&zero, 1);
		uint8_t len[8];
		for (int i = 0; i < 8; i++)
			len[i] = (uint8_t)(bits >> (56 - 8 * i));
		Update(len, 8);
		char out[65];
		for (int i = 0; i < 8; i++)
			snprintf(out + i * 8, 9, "%08x", h_[i]);
		return std::string(out, 64);
	}

private:
	void Reset() {
		static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
		memcpy(h_, init, sizeof(h_));
		fill_ = 0;
		total_ = 0;
	}

	static uint32_t Ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

	void Block(const uint8_t *p) {
		static const uint32_t k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
		for (int i = 0; i < 64; i++) {
			uint32_t t1 = h + (Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			uint32_t t2 = (Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
	}

	uint32_t h_[8];
	uint8_t buf_[64];
	size_t fill_;
	uint64_t total_;
};

std::string Sha(const uint8_t *p, size_t n) {
	Sha256 s;
	s.Update(p, n);
	return s.Final();
}
std::string Sha(const Bytes &b) { return Sha(b.data(), b.size()); }

// UTF-8 / UTF-16 ------------------------------------------------------------

void AppendUtf8(std::string *out, uint32_t cp) {
	if (cp < 0x80) {
		out->push_back((char)cp);
	} else if (cp < 0x800) {
		out->push_back((char)(0xC0 | (cp >> 6)));
		out->push_back((char)(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out->push_back((char)(0xE0 | (cp >> 12)));
		out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
		out->push_back((char)(0x80 | (cp & 0x3F)));
	} else {
		out->push_back((char)(0xF0 | (cp >> 18)));
		out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
		out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
		out->push_back((char)(0x80 | (cp & 0x3F)));
	}
}

// Equivalent of bytes.decode("utf-8", "replace").
std::string Utf8Replace(const uint8_t *s, size_t n) {
	std::string out;
	out.reserve(n);
	size_t i = 0;
	while (i < n) {
		uint8_t b = s[i];
		if (b < 0x80) {
			out.push_back((char)b);
			i++;
			continue;
		}
		int need = 0;
		uint8_t lo = 0x80, hi = 0xBF;
		if (b >= 0xC2 && b <= 0xDF) {
			need = 1;
		} else if (b >= 0xE0 && b <= 0xEF) {
			need = 2;
			if (b == 0xE0) lo = 0xA0;
			if (b == 0xED) hi = 0x9F;
		} else if (b >= 0xF0 && b <= 0xF4) {
			need = 3;
			if (b == 0xF0) lo = 0x90;
			if (b == 0xF4) hi = 0x8F;
		} else {
			out += "\xEF\xBF\xBD";
			i++;
			continue;
		}
		size_t j = i + 1;
		int got = 0;
		while (got < need && j < n && s[j] >= lo && s[j] <= hi) {
			j++;
			got++;
			lo = 0x80;
			hi = 0xBF;
		}
		if (got == need)
			out.append(reinterpret_cast<const char *>(s + i), j - i);
		else
			out += "\xEF\xBF\xBD";
		i = j;
	}
	return out;
}

// Equivalent of bytes.decode("utf-16-le", "replace").
std::string Utf16LeReplace(const uint8_t *s, size_t n) {
	std::string out;
	size_t i = 0;
	while (i + 1 < n) {
		uint32_t u = s[i] | (s[i + 1] << 8);
		i += 2;
		if (u >= 0xD800 && u <= 0xDBFF) {
			if (i + 1 < n) {
				uint32_t v = s[i] | (s[i + 1] << 8);
				if (v >= 0xDC00 && v <= 0xDFFF) {
					i += 2;
					AppendUtf8(&out, 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00));
					continue;
				}
			}
			AppendUtf8(&out, 0xFFFD);
		} else if (u >= 0xDC00 && u <= 0xDFFF) {
			AppendUtf8(&out, 0xFFFD);
		} else {
			AppendUtf8(&out, u);
		}
	}
	if (i < n)
		AppendUtf8(&out, 0xFFFD);
	return out;
}

// JSON ----------------------------------------------------------------------

std::string JStr(const std::string &utf8) {
	std::string out = "\"";
	size_t i = 0;
	char buf[16];
	auto put16 = [&](uint32_t u) {
		snprintf(buf, sizeof(buf), "\\u%04x", u);
		out += buf;
	};
	while (i < utf8.size()) {
		uint8_t b = (uint8_t)utf8[i];
		uint32_t cp;
		int len;
		if (b < 0x80) { cp = b; len = 1; }
		else if (b < 0xE0) { cp = b & 0x1F; len = 2; }
		else if (b < 0xF0) { cp = b & 0x0F; len = 3; }
		else { cp = b & 0x07; len = 4; }
		for (int k = 1; k < len && i + k < utf8.size(); k++)
			cp = (cp << 6) | ((uint8_t)utf8[i + k] & 0x3F);
		i += len;
		switch (cp) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		default:
			if (cp < 0x20 || (cp >= 0x80 && cp < 0x10000)) {
				put16(cp);
			} else if (cp >= 0x10000) {
				cp -= 0x10000;
				put16(0xD800 + (cp >> 10));
				put16(0xDC00 + (cp & 0x3FF));
			} else {
				out.push_back((char)cp);
			}
		}
	}
	out += "\"";
	return out;
}

std::string JNum(uint64_t v) { return std::to_string(v); }

std::string JArr(const std::vector<std::string> &items) {
	std::string out = "[";
	for (size_t i = 0; i < items.size(); i++) {
		out += i ? ",\n        " : "\n        ";
		out += JStr(items[i]);
	}
	out += "\n      ]";
	return out;
}

struct Record {
	std::string file;
	std::vector<std::pair<std::string, std::string>> fields;  // value already JSON
	void Add(const char *key, std::string value) { fields.emplace_back(key, std::move(value)); }
};

// ---------------------------------------------------------------- RCO

constexpr uint32_t kRcoMagic = 0x46525000;
constexpr size_t kRcoHeaderSize = 0xA4;
constexpr size_t kEntryHeaderSize = 0x28;
constexpr uint32_t kNone = 0xFFFFFFFFu;

enum {
	kTableText = 3,
	kTableImage = 4,
	kTableModel = 5,
	kTableSound = 6,
	kTableFont = 7,
	kTableObject = 8,
	kTableAnim = 9,
};

uint32_t Le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
uint32_t Le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

struct Entry {
	uint32_t offset = 0, type_id = 0, label_offset = 0, entry_size = 0, next_offset = 0;
	const uint8_t *extra = nullptr;
	size_t extra_len = 0;
	std::vector<int> children;
	int table_id() const { return (type_id >> 8) & 0xFF; }
	int type() const { return type_id & 0xFF; }
};

struct Payload {
	std::string label;
	uint32_t format = 0, compression = 0, packed_size = 0, unpacked_size = 0, offset = 0;
	Bytes data;
};

struct SoundChannel {
	Bytes data;
};

struct Sound {
	std::string label;
	uint32_t format = 0, offset = 0;
	std::vector<SoundChannel> channels;
};

class Rco {
public:
	Rco(std::string name, Bytes data) : name_(std::move(name)), data_(std::move(data)) {
		ParseHeader();
		ReadLabels();
		int root = ReadEntry(h_.p_main, 0);
		for (int child : entries_[root].children) {
			if (entries_[child].type() == 0 && !tables_.count(entries_[child].table_id()))
				tables_[entries_[child].table_id()] = child;
		}
	}

	const std::vector<Entry> &entries() const { return entries_; }

	std::vector<int> TableChildren(int table) const {
		auto it = tables_.find(table);
		return it == tables_.end() ? std::vector<int>() : entries_[it->second].children;
	}

	// Payload-carrying image (table 4) or model (table 5) entries.
	template <typename Fn>
	void ForEachBlob(int table, const char *what, Fn fn) {
		const uint32_t data_base = table == kTableImage ? h_.p_image_data : h_.p_model_data;
		for (int idx : TableChildren(table)) {
			const Entry &e = entries_[idx];
			if (e.type() != 1 || e.extra_len < 12)
				continue;
			Payload p;
			p.format = Le16(e.extra);
			p.compression = Le16(e.extra + 2);
			p.packed_size = Le32(e.extra + 4);
			p.offset = Le32(e.extra + 8);
			p.unpacked_size = p.packed_size;
			if (p.compression && e.extra_len >= 16)
				p.unpacked_size = Le32(e.extra + 12);
			if (p.packed_size == 0 || data_base == kNone)
				continue;
			uint64_t start = (uint64_t)data_base + p.offset;
			uint64_t end = start + p.packed_size;
			if (end > data_.size())
				throw PspError(name_ + ": " + what + " payload outside the file");
			const uint8_t *src = data_.data() + start;
			if (p.compression == 1) {
				if (!ZlibInflate(src, p.packed_size, &p.data))
					throw PspError(name_ + ": invalid zlib " + what + " payload");
			} else if (p.compression == 2) {
				p.data = RlzDecompress(src, p.packed_size, p.unpacked_size);
			} else if (p.compression == 0) {
				p.data.assign(src, src + p.packed_size);
			} else {
				throw PspError(name_ + ": " + what + " compression " + std::to_string(p.compression));
			}
			p.label = Label(e.label_offset);
			if (p.label.empty())
				p.label = std::string(what) + "_" + Hex6(p.offset);
			fn(p);
		}
	}

	std::vector<Sound> Sounds() {
		std::vector<Sound> out;
		for (int idx : TableChildren(kTableSound)) {
			const Entry &e = entries_[idx];
			if (e.type() != 1 || e.extra_len < 12)
				continue;
			Sound s;
			s.format = Le16(e.extra);
			uint32_t channels = Le16(e.extra + 2);
			s.offset = Le32(e.extra + 8);
			for (uint32_t i = 0; i < std::min<uint32_t>(channels, 2); i++) {
				if (12 + (size_t)i * 8 + 8 > e.extra_len)
					throw PspError(name_ + ": sound entry too short");
				uint32_t size = Le32(e.extra + 12 + i * 8);
				uint32_t offset = Le32(e.extra + 16 + i * 8);
				if (size == 0 || h_.p_sound_data == kNone)
					continue;
				uint64_t start = (uint64_t)h_.p_sound_data + offset;
				if (start + size > data_.size())
					throw PspError(name_ + ": sound payload outside the file");
				SoundChannel c;
				c.data.assign(data_.begin() + start, data_.begin() + start + size);
				s.channels.push_back(std::move(c));
			}
			s.label = Label(e.label_offset);
			if (s.label.empty())
				s.label = "sound_" + Hex6(s.offset);
			out.push_back(std::move(s));
		}
		return out;
	}

	// All English-style strings of one language from the text tables.
	void TextStrings(int lang, std::map<std::string, std::string> *strings) {
		for (const Entry &e : entries_) {
			if (e.table_id() != kTableText || e.type() != 1 || e.extra_len < 8)
				continue;
			if ((int)Le16(e.extra) != lang)
				continue;
			const uint32_t text_format = Le16(e.extra + 2);
			const uint32_t count = Le32(e.extra + 4);
			const Bytes *block = TextBlock(lang);
			if (!block)
				continue;
			const size_t n = std::min<size_t>(count, (e.extra_len - 8) / 12);
			for (size_t i = 0; i < n; i++) {
				uint32_t label_offset = Le32(e.extra + 8 + i * 12);
				uint32_t length = Le32(e.extra + 12 + i * 12);
				uint32_t offset = Le32(e.extra + 16 + i * 12);
				if ((uint64_t)offset + length > block->size())
					continue;
				const uint8_t *raw = block->data() + offset;
				std::string text = text_format == 1 ? Utf16LeReplace(raw, length) : Utf8Replace(raw, length);
				std::string label = Label(label_offset);
				if (label.empty()) {
					char buf[24];
					snprintf(buf, sizeof(buf), "text_%03zu", i);
					label = buf;
				}
				while (!text.empty() && text.back() == '\0')
					text.pop_back();
				(*strings)[label] = std::move(text);
			}
		}
	}

private:
	struct Header {
		uint32_t compression, p_main, p_text_data, l_text_data, p_label_data, l_label_data;
		uint32_t p_image_data, p_sound_data, p_model_data;
	};

	void ParseHeader() {
		const uint8_t *d = data_.data();
		if (data_.size() < kRcoHeaderSize)
			throw PspError(name_ + ": shorter than an RCO header");
		if (Le32(d) != kRcoMagic)
			throw PspError(name_ + ": not a PSP RCO");
		h_.compression = Le32(d + 12);
		h_.p_main = Le32(d + 0x10);
		h_.p_text_data = Le32(d + 0x38);
		h_.l_text_data = Le32(d + 0x3C);
		h_.p_label_data = Le32(d + 0x40);
		h_.l_label_data = Le32(d + 0x44);
		h_.p_image_data = Le32(d + 0x80);
		h_.p_sound_data = Le32(d + 0x88);
		h_.p_model_data = Le32(d + 0x90);
		method_ = (h_.compression >> 4) & 0xF;
		table_base_ = kRcoHeaderSize;
		if (method_ == 1 || method_ == 2) {
			if (data_.size() < kRcoHeaderSize + 12)
				throw PspError(name_ + ": truncated table stream");
			uint32_t packed = Le32(d + kRcoHeaderSize);
			uint32_t unpacked = Le32(d + kRcoHeaderSize + 4);
			if (kRcoHeaderSize + 12 + (uint64_t)packed > data_.size())
				throw PspError(name_ + ": truncated table stream");
			const uint8_t *blob = d + kRcoHeaderSize + 12;
			if (method_ == 1) {
				if (!ZlibInflate(blob, packed, &table_owned_))
					throw PspError(name_ + ": invalid zlib table stream");
				if (table_owned_.size() != unpacked)
					throw PspError(name_ + ": zlib table size mismatch");
			} else {
				table_owned_ = RlzDecompress(blob, packed, unpacked);
			}
			table_ = table_owned_.data();
			table_len_ = table_owned_.size();
		} else if (method_ == 0) {
			table_base_ = 0;
			table_ = d;
			table_len_ = data_.size();
		} else {
			throw PspError(name_ + ": unknown table compression " + std::to_string(method_));
		}
	}

	const uint8_t *TableRead(uint64_t offset, uint64_t size) const {
		if (offset < table_base_ || offset - table_base_ + size > table_len_)
			throw PspError(name_ + ": table read outside decompressed data");
		return table_ + (offset - table_base_);
	}

	void ReadLabels() {
		if (h_.p_label_data == kNone || !h_.l_label_data)
			return;
		const uint8_t *p = TableRead(h_.p_label_data, h_.l_label_data);
		labels_.assign(p, p + h_.l_label_data);
	}

	std::string Label(uint32_t offset) const {
		if (offset == kNone || offset == 0 || offset >= labels_.size())
			return "";
		size_t end = offset;
		while (end < labels_.size() && labels_[end])
			end++;
		return Utf8Replace(labels_.data() + offset, end - offset);
	}

	size_t ExtraSize(const Entry &e) const {
		const int table = e.table_id();
		if (e.type() == 0)
			return 0;
		if (table == kTableImage || table == kTableModel) {
			if (e.type() != 1)
				return 0;
			if (e.next_offset && e.next_offset < kEntryHeaderSize + 0x10)
				return 0x0C;
			return 0x10;
		}
		if (table == kTableSound && e.type() == 1) {
			uint32_t channels = Le16(TableRead((uint64_t)e.offset + kEntryHeaderSize + 2, 2));
			return 0x0C + (size_t)std::max<uint32_t>(2, channels) * 8;
		}
		if (table == kTableText && e.type() == 1) {
			uint32_t count = Le32(TableRead((uint64_t)e.offset + kEntryHeaderSize + 4, 4));
			return 0x08 + (size_t)count * 12;
		}
		if (table == kTableFont && e.type() == 1)
			return 0x0C;
		if (table == kTableObject && e.type() > 0) {
			if (e.entry_size >= kEntryHeaderSize)
				return e.entry_size - kEntryHeaderSize;
			if (e.next_offset >= kEntryHeaderSize)
				return e.next_offset - kEntryHeaderSize;
			return 0;
		}
		if (table == kTableAnim && e.type() > 1 && e.next_offset >= kEntryHeaderSize)
			return e.next_offset - kEntryHeaderSize;
		return 0;
	}

	int ReadEntry(uint64_t offset, int depth) {
		if (depth > 24)
			throw PspError(name_ + ": entry tree deeper than 24 levels");
		const uint8_t *h = TableRead(offset, kEntryHeaderSize);
		Entry e;
		e.offset = (uint32_t)offset;
		e.type_id = Le16(h);
		e.label_offset = Le32(h + 4);
		e.entry_size = Le32(h + 12);
		const uint32_t subentry_count = Le32(h + 16);
		e.next_offset = Le32(h + 20);
		const size_t extra_size = ExtraSize(e);
		if (extra_size) {
			e.extra = TableRead(offset + kEntryHeaderSize, extra_size);
			e.extra_len = extra_size;
		}
		const int index = (int)entries_.size();
		entries_.push_back(e);
		std::vector<int> children;
		if (subentry_count) {
			uint64_t position = offset + kEntryHeaderSize + extra_size;
			for (uint32_t i = 0; i < subentry_count; i++) {
				int child = ReadEntry(position, depth + 1);
				children.push_back(child);
				if (i + 1 < subentry_count) {
					if (entries_[child].next_offset == 0)
						break;
					position = (uint64_t)entries_[child].offset + entries_[child].next_offset;
				}
			}
		}
		entries_[index].children = std::move(children);
		return index;
	}

	// Decoded text pool of one language (last valid block wins), or null.
	const Bytes *TextBlock(int lang) {
		auto cached = text_cache_.find(lang);
		if (cached != text_cache_.end())
			return cached->second.first ? &cached->second.second : nullptr;
		Bytes result;
		bool present = false;
		const uint32_t start = h_.p_text_data;
		if (start != kNone && h_.l_text_data != 0) {
			uint64_t pos = start;
			const uint64_t end = std::min<uint64_t>(data_.size(), (uint64_t)start + h_.l_text_data);
			while (pos + 16 <= end) {
				const uint8_t *p = data_.data() + pos;
				uint32_t block_lang = Le16(p);
				uint32_t next_offset = Le32(p + 4);
				uint32_t packed = Le32(p + 8);
				uint32_t unpacked = Le32(p + 12);
				if (pos + 16 + packed > data_.size())
					break;
				if ((int)block_lang == lang) {
					const uint8_t *payload = p + 16;
					Bytes block;
					if (method_ == 2)
						block = RlzDecompress(payload, packed, unpacked);
					else if (method_ == 1) {
						if (!ZlibInflate(payload, packed, &block))
							throw PspError(name_ + ": invalid zlib text block");
					} else
						block.assign(payload, payload + packed);
					if (block.size() == unpacked) {
						result = std::move(block);
						present = true;
					}
				}
				if (next_offset == 0)
					break;
				pos = (uint64_t)start + next_offset;
			}
		}
		auto &slot = text_cache_[lang];
		slot.first = present;
		slot.second = std::move(result);
		return present ? &slot.second : nullptr;
	}

	std::string name_;
	Bytes data_;
	Header h_{};
	int method_ = 0;
	uint64_t table_base_ = 0;
	Bytes table_owned_;
	const uint8_t *table_ = nullptr;
	size_t table_len_ = 0;
	Bytes labels_;
	std::vector<Entry> entries_;
	std::map<int, int> tables_;
	std::map<int, std::pair<bool, Bytes>> text_cache_;
};

// ---------------------------------------------------------------- extraction

const char *const kRcoFiles[] = {"system_plugin.rco", "system_plugin_bg.rco", "system_plugin_fg.rco",
                                 "topmenu_plugin.rco", "topmenu_icon.rco",    "game_plugin.rco",
                                 "savedata_plugin.rco", "opening_plugin.rco", "impose_plugin.rco",
                                 "sysconf_plugin.rco", "netconf_dialog.rco"};

std::string SafeName(const std::string &label, const std::string &fallback) {
	// Non-ASCII code points count as non-alphanumeric (the firmware labels are ASCII).
	std::string cleaned;
	for (size_t i = 0; i < label.size();) {
		uint8_t c = (uint8_t)label[i];
		if (c >= 0x80) {
			i++;
			while (i < label.size() && ((uint8_t)label[i] & 0xC0) == 0x80)
				i++;
			cleaned.push_back('_');
			continue;
		}
		i++;
		if (isalnum(c) || strchr("-_.()", c))
			cleaned.push_back((char)c);
		else
			cleaned.push_back('_');
	}
	size_t b = cleaned.find_first_not_of("._");
	if (b == std::string::npos)
		return fallback;
	size_t e = cleaned.find_last_not_of("._");
	return cleaned.substr(b, e - b + 1);
}

struct Extractor {
	fs::path flash0, resource_dir, out;
	std::vector<Record> files;
	uint64_t images = 0, audio = 0, models = 0, fonts = 0, raw = 0, rcos = 0, texts = 0;
	bool have_text = false;

	std::string Rel(const fs::path &p) const { return p.lexically_relative(out).generic_string(); }

	std::string Version() const {
		fs::path p = flash0 / "vsh" / "etc" / "version.txt";
		std::error_code ec;
		if (!fs::is_regular_file(p, ec))
			return "unknown";
		Bytes raw_text = ReadAll(p);
		std::string text = Utf8Replace(raw_text.data(), raw_text.size());
		auto strip = [](std::string s) {
			const char *ws = " \t\n\r\v\f\x1c\x1d\x1e\x1f";
			size_t b = s.find_first_not_of(ws);
			if (b == std::string::npos)
				return std::string();
			return s.substr(b, s.find_last_not_of(ws) - b + 1);
		};
		size_t i = 0;
		while (i <= text.size()) {
			size_t j = text.find_first_of("\r\n\v\f\x1c\x1d\x1e", i);
			std::string line = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
			size_t colon = line.find(':');
			if (colon != std::string::npos) {
				if (strip(line.substr(0, colon)) == "release")
					return strip(line.substr(colon + 1));
			} else if (!strip(line).empty()) {
				return strip(line);
			}
			if (j == std::string::npos)
				break;
			i = j + ((text[j] == '\r' && j + 1 < text.size() && text[j + 1] == '\n') ? 2 : 1);
		}
		return "unknown";
	}

	void Run() {
		std::error_code ec;
		if (!fs::is_directory(resource_dir, ec))
			throw PspError("no vsh/resource under " + flash0.string());
		for (const char *d : {"images", "audio", "fonts", "models", "rco", "text", "images/themes"})
			fs::create_directories(out / d, ec);

		std::map<std::string, std::string> strings;
		for (const char *name : kRcoFiles) {
			fs::path path = resource_dir / name;
			if (!fs::is_regular_file(path, ec))
				continue;
			ExtractRco(name, path, &strings);
		}

		fs::path font_dir = flash0 / "font";
		for (int i = 0; i <= 15; i++) {
			std::string font = "ltn" + std::to_string(i) + ".pgf";
			fs::path source = font_dir / font;
			if (!fs::is_regular_file(source, ec))
				continue;
			Bytes data = ReadAll(source);
			fs::path dest = out / "fonts" / font;
			WriteAll(dest, data);
			Record r;
			r.file = Rel(dest);
			r.Add("kind", JStr("font"));
			r.Add("file", JStr(r.file));
			r.Add("sha256_file", JStr(Sha(data)));
			r.Add("sha256_source", JStr(Sha(data)));
			files.push_back(std::move(r));
			fonts++;
		}

		if (!strings.empty()) {
			std::string body;
			for (auto &kv : strings) {
				body += kv.first;
				body += '\t';
				for (char c : kv.second) {
					if (c == '\n')
						body += "\\n";
					else
						body.push_back(c);
				}
				body += '\n';
			}
			fs::path dest = out / "text" / "en.txt";
			WriteAll(dest, reinterpret_cast<const uint8_t *>(body.data()), body.size());
			Record r;
			r.file = Rel(dest);
			r.Add("kind", JStr("text"));
			r.Add("language", JStr("en"));
			r.Add("count", JNum(strings.size()));
			r.Add("file", JStr(r.file));
			r.Add("sha256_file", JStr(Sha(reinterpret_cast<const uint8_t *>(body.data()), body.size())));
			files.push_back(std::move(r));
			texts = strings.size();
			have_text = true;
		}

		fs::path background = resource_dir / "system_plugin_bg.rco";
		if (fs::is_regular_file(background, ec)) {
			Bytes data = ReadAll(background);
			fs::path dest = out / "rco" / "system_plugin_bg.rco";
			WriteAll(dest, data);
			Record r;
			r.file = Rel(dest);
			r.Add("kind", JStr("rco"));
			r.Add("file", JStr(r.file));
			r.Add("sha256_file", JStr(Sha(data)));
			r.Add("sha256_source", JStr(Sha(data)));
			files.push_back(std::move(r));
			rcos++;
		}

		ExtractThemes();

		if (images == 0)
			throw PspError("no images were extracted from " + resource_dir.string());
		WriteManifest();
	}

	fs::path Target(std::map<std::string, int> *used, const char *kind, const std::string &stem,
	                const std::string &label, uint32_t offset, const char *suffix) {
		std::string base = SafeName(label, std::string(kind) + "_" + Hex6(offset));
		int &n = (*used)[std::string(kind) + "/" + base];
		n++;
		if (n > 1)
			base += "_" + std::to_string(n);
		return out / kind / stem / (base + suffix);
	}

	void ExtractRco(const std::string &name, const fs::path &path, std::map<std::string, std::string> *strings) {
		Rco rco(name, ReadAll(path));
		const std::string stem = fs::path(name).stem().string();
		std::map<std::string, int> used;

		rco.ForEachBlob(kTableImage, "image", [&](Payload &img) {
			fs::path dest = Target(&used, "images", stem, img.label, img.offset, ".png");
			Record r;
			r.Add("kind", JStr("image"));
			r.Add("rco", JStr(name));
			r.Add("label", JStr(img.label));
			r.Add("format", JNum(img.format));
			r.Add("compression", JNum(img.compression));
			r.Add("packed_size", JNum(img.packed_size));
			r.Add("unpacked_size", JNum(img.unpacked_size));
			r.Add("sha256_source", JStr(Sha(img.data)));
			try {
				GimImage decoded = GimDecode(img.data.data(), img.data.size());
				Bytes png = PngEncode(decoded.width, decoded.height, decoded.rgba.data());
				WriteAll(dest, png);
				r.Add("width", JNum(decoded.width));
				r.Add("height", JNum(decoded.height));
				r.file = Rel(dest);
				r.Add("file", JStr(r.file));
				r.Add("sha256_file", JStr(Sha(png)));
				images++;
			} catch (const PspError &error) {
				dest.replace_extension(".gim");
				WriteAll(dest, img.data);
				r.Add("error", JStr(error.what()));
				r.file = Rel(dest);
				r.Add("file", JStr(r.file));
				r.Add("sha256_file", JStr(Sha(img.data)));
				raw++;
			}
			files.push_back(std::move(r));
		});

		for (Sound &sound : rco.Sounds()) {
			std::vector<VagSound> decoded;
			for (SoundChannel &c : sound.channels)
				decoded.push_back(VagDecode(c.data.data(), c.data.size()));
			if (decoded.empty())
				continue;
			Bytes wav;
			uint32_t rate;
			size_t frames;
			if (decoded.size() == 1) {
				wav = WavBuild(decoded[0].sample_rate, 1, decoded[0].samples);
				rate = decoded[0].sample_rate;
				frames = decoded[0].samples.size();
			} else {
				frames = std::min(decoded[0].samples.size(), decoded[1].samples.size());
				std::vector<int16_t> merged(frames * 2);
				for (size_t i = 0; i < frames; i++) {
					merged[i * 2] = decoded[0].samples[i];
					merged[i * 2 + 1] = decoded[1].samples[i];
				}
				rate = decoded[0].sample_rate;
				wav = WavBuild(rate, 2, merged);
			}
			fs::path dest = Target(&used, "audio", stem, sound.label, sound.offset, ".wav");
			WriteAll(dest, wav);
			Record r;
			r.file = Rel(dest);
			std::vector<std::string> sources;
			for (SoundChannel &c : sound.channels)
				sources.push_back(Sha(c.data));
			r.Add("kind", JStr("audio"));
			r.Add("rco", JStr(name));
			r.Add("label", JStr(sound.label));
			r.Add("format", JNum(sound.format));
			r.Add("channels", JNum(decoded.size()));
			r.Add("sample_rate", JNum(rate));
			r.Add("frames", JNum(frames));
			r.Add("file", JStr(r.file));
			r.Add("sha256_file", JStr(Sha(wav)));
			r.Add("sha256_source", JArr(sources));
			files.push_back(std::move(r));
			audio++;
		}

		rco.ForEachBlob(kTableModel, "model", [&](Payload &model) {
			fs::path dest = Target(&used, "models", stem, model.label, model.offset, ".gmo");
			WriteAll(dest, model.data);
			Record r;
			r.file = Rel(dest);
			r.Add("kind", JStr("model"));
			r.Add("rco", JStr(name));
			r.Add("label", JStr(model.label));
			r.Add("format", JNum(model.format));
			r.Add("compression", JNum(model.compression));
			r.Add("packed_size", JNum(model.packed_size));
			r.Add("unpacked_size", JNum(model.unpacked_size));
			r.Add("file", JStr(r.file));
			std::string sha = Sha(model.data);
			r.Add("sha256_file", JStr(sha));
			r.Add("sha256_source", JStr(sha));
			files.push_back(std::move(r));
			models++;
		});

		if (name == "topmenu_plugin.rco" || name == "game_plugin.rco")
			rco.TextStrings(1, strings);
	}

	// ---- themes / gameboot

	void ThemePng(const std::string &name, int w, int h, const Bytes &png, const std::string &source_digest) {
		fs::path dest = out / "images" / "themes" / (name + ".png");
		WriteAll(dest, png);
		Record r;
		r.file = Rel(dest);
		r.Add("kind", JStr("image"));
		r.Add("rco", JStr("theme"));
		r.Add("label", JStr(name));
		r.Add("width", JNum(w));
		r.Add("height", JNum(h));
		r.Add("file", JStr(r.file));
		r.Add("sha256_file", JStr(Sha(png)));
		r.Add("sha256_source", JStr(source_digest));
		files.push_back(std::move(r));
		images++;
	}

	static Bytes Bmp24ToPng(const uint8_t *data, size_t size, int *w, int *h) {
		if (size < 54 || data[0] != 'B' || data[1] != 'M')
			throw PspError("not a BMP");
		uint32_t pixel_offset = Le32(data + 10);
		int32_t width = (int32_t)Le32(data + 18);
		int32_t raw_height = (int32_t)Le32(data + 22);
		uint32_t bpp = Le16(data + 28);
		if (width <= 0 || raw_height == 0 || bpp != 24)
			throw PspError("unsupported BMP " + std::to_string(width) + "x" + std::to_string(raw_height) + " " +
			               std::to_string(bpp) + "bpp");
		bool top_down = raw_height < 0;
		int64_t height = raw_height < 0 ? -(int64_t)raw_height : raw_height;
		uint64_t stride = ((uint64_t)width * 3 + 3) & ~(uint64_t)3;
		if ((uint64_t)pixel_offset + stride * height > size)
			throw PspError("BMP pixel data outside the file");
		Bytes rgba((size_t)width * height * 4);
		for (int64_t y = 0; y < height; y++) {
			int64_t src_y = top_down ? y : height - 1 - y;
			const uint8_t *row = data + pixel_offset + src_y * stride;
			uint8_t *dst = &rgba[(size_t)y * width * 4];
			for (int32_t x = 0; x < width; x++) {
				dst[x * 4] = row[x * 3 + 2];
				dst[x * 4 + 1] = row[x * 3 + 1];
				dst[x * 4 + 2] = row[x * 3];
				dst[x * 4 + 3] = 255;
			}
		}
		*w = width;
		*h = (int)height;
		return PngEncode(width, (int)height, rgba.data());
	}

	void BmpStrip(const char *src_name, const char *prefix, int expected) {
		std::error_code ec;
		fs::path src = resource_dir / src_name;
		if (!fs::is_regular_file(src, ec))
			return;
		Bytes raw_data = ReadAll(src);
		const size_t chunk_size = 6176;  // 60x34 24bpp BMP (6174 + 2-byte alignment)
		size_t count = raw_data.size() / chunk_size;
		for (size_t i = 0; i < std::min<size_t>(count, expected); i++) {
			const uint8_t *chunk = raw_data.data() + i * chunk_size;
			if (chunk[0] != 'B' || chunk[1] != 'M')
				continue;
			int w, h;
			Bytes png = Bmp24ToPng(chunk, chunk_size, &w, &h);
			char name[32];
			snprintf(name, sizeof(name), "%s_%02zu", prefix, i);
			ThemePng(name, w, h, png, Sha(chunk, chunk_size));
		}
	}

	static std::ptrdiff_t Find2(const uint8_t *p, size_t n, size_t from) {
		for (size_t i = from; i + 1 < n; i++)
			if (p[i] == 0x78 && p[i + 1] == 0x9c)
				return (std::ptrdiff_t)i;
		return -1;
	}

	void ExtractThemes() {
		std::error_code ec;
		BmpStrip(fs::is_regular_file(resource_dir / "01-12_03g.bmp", ec) ? "01-12_03g.bmp" : "01-12.bmp", "orig", 12);
		BmpStrip("01-12.bmp", "classic", 12);
		BmpStrip("13-27.bmp", "ext", 22);

		fs::path ptf_path = resource_dir / "custom_theme.dat";
		if (fs::is_regular_file(ptf_path, ec)) {
			Bytes ptf = ReadAll(ptf_path);
			if (ptf.size() >= 0x114 && ptf[0] == 0 && memcmp(&ptf[1], "PTF", 3) == 0) {
				uint32_t offs[5];
				for (int i = 0; i < 5; i++)
					offs[i] = Le32(&ptf[0x100 + i * 4]);
				auto section = [&](uint32_t a, uint32_t b) {
					size_t s = std::min<size_t>(a, ptf.size()), e = std::min<size_t>(b, ptf.size());
					return e > s ? Bytes(ptf.begin() + s, ptf.begin() + e) : Bytes();
				};
				Bytes sec1 = section(offs[1], offs[2]);
				std::ptrdiff_t zp = Find2(sec1.data(), sec1.size(), 0);
				if (zp >= 0) {
					Bytes bmp;
					if (!ZlibInflate(sec1.data() + zp, sec1.size() - zp, &bmp))
						throw PspError("invalid zlib stream in custom_theme.dat");
					int w, h;
					Bytes png = Bmp24ToPng(bmp.data(), bmp.size(), &w, &h);
					ThemePng("cookies_bg", w, h, png, Sha(bmp));
				}
				Bytes sec2 = section(offs[2], offs[3]);
				std::vector<Bytes> gims;
				size_t pos = 0;
				while (gims.size() < 8) {
					std::ptrdiff_t z = Find2(sec2.data(), sec2.size(), pos);
					if (z < 0)
						break;
					Bytes dec;
					if (ZlibInflate(sec2.data() + z, sec2.size() - z, &dec) && dec.size() >= 4 &&
					    memcmp(dec.data(), "MIG.", 4) == 0)
						gims.push_back(std::move(dec));
					pos = (size_t)z + 2;
				}
				static const int ptf_to_xmb[8] = {1, 7, 2, 3, 4, 5, 6, 0};
				if (gims.size() == 8) {
					for (int cat = 0; cat < 8; cat++) {
						const Bytes &g = gims[ptf_to_xmb[cat]];
						GimImage decoded = GimDecode(g.data(), g.size());
						Bytes png = PngEncode(decoded.width, decoded.height, decoded.rgba.data());
						ThemePng("cookies_cat_" + std::to_string(cat), decoded.width, decoded.height, png, Sha(g));
					}
				}
			}
		}

		fs::path pmf_path = resource_dir / "gameboot.pmf";
		if (fs::is_regular_file(pmf_path, ec)) {
			Bytes pmf = ReadAll(pmf_path);
			Bytes stream;
			size_t pos = 0;
			static const uint8_t marker[4] = {0x00, 0x00, 0x01, 0xbd};
			for (;;) {
				auto it = std::search(pmf.begin() + std::min(pos, pmf.size()), pmf.end(), marker, marker + 4);
				if (it == pmf.end())
					break;
				size_t idx = (size_t)(it - pmf.begin());
				if (idx + 9 > pmf.size())
					break;
				size_t pkt_len = ((size_t)pmf[idx + 4] << 8) | pmf[idx + 5];
				size_t hdr_len = pmf[idx + 8];
				size_t payload_start = idx + 9 + hdr_len;
				size_t payload_end = std::min(pmf.size(), idx + 6 + pkt_len);
				if (payload_start + 4 < payload_end)
					stream.insert(stream.end(), pmf.begin() + payload_start + 4, pmf.begin() + payload_end);
				pos = idx + 4;
			}
			Bytes frames;
			for (size_t off = 0; off < stream.size(); off += 752) {
				if (off + 752 <= stream.size() && stream[off] == 0x0f && stream[off + 1] == 0xd0 &&
				    stream[off + 2] == 0x28 && stream[off + 3] == 0x5c)
					frames.insert(frames.end(), stream.begin() + off + 8, stream.begin() + off + 752);
			}
			if (!frames.empty()) {
				const uint32_t block_align = 744, sample_rate = 44100, channels = 2;
				const uint32_t avg_bps = (sample_rate * block_align) / 2048;
				Bytes fmt;
				auto le16 = [](Bytes *b, uint32_t v) { b->push_back(v & 0xFF); b->push_back((v >> 8) & 0xFF); };
				auto le32 = [&](Bytes *b, uint32_t v) { le16(b, v & 0xFFFF); le16(b, v >> 16); };
				le16(&fmt, 0xFFFE);
				le16(&fmt, channels);
				le32(&fmt, sample_rate);
				le32(&fmt, avg_bps);
				le16(&fmt, block_align);
				le16(&fmt, 0);
				le16(&fmt, 0);
				Bytes body = {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
				le32(&body, (uint32_t)fmt.size());
				body.insert(body.end(), fmt.begin(), fmt.end());
				body.insert(body.end(), {'d', 'a', 't', 'a'});
				le32(&body, (uint32_t)frames.size());
				body.insert(body.end(), frames.begin(), frames.end());
				Bytes at3 = {'R', 'I', 'F', 'F'};
				le32(&at3, (uint32_t)body.size());
				at3.insert(at3.end(), body.begin(), body.end());
				fs::path dest = out / "audio" / "system_plugin" / "snd_gameboot.at3";
				WriteAll(dest, at3);
				Record r;
				r.file = Rel(dest);
				r.Add("kind", JStr("audio"));
				r.Add("rco", JStr("gameboot.pmf"));
				r.Add("label", JStr("snd_gameboot"));
				r.Add("format", JStr("atrac3plus"));
				r.Add("channels", JNum(channels));
				r.Add("sample_rate", JNum(sample_rate));
				r.Add("frames", JNum(frames.size() / block_align));
				r.Add("file", JStr(r.file));
				r.Add("sha256_file", JStr(Sha(at3)));
				r.Add("sha256_source", JStr(Sha(pmf)));
				files.push_back(std::move(r));
				audio++;
			}
		}
	}

	void WriteManifest() {
		std::stable_sort(files.begin(), files.end(), [](const Record &a, const Record &b) { return a.file < b.file; });
		time_t now = time(nullptr);
		struct tm tmv;
#ifdef _WIN32
		gmtime_s(&tmv, &now);
#else
		gmtime_r(&now, &tmv);
#endif
		char stamp[32];
		strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tmv);

		std::string m = "{\n  \"schema\": 1,\n";
		m += std::string("  \"generated_utc\": ") + JStr(stamp) + ",\n";
		m += "  \"firmware\": " + JStr(Version()) + ",\n";
		m += "  \"counts\": {\n";
		m += "    \"images\": " + JNum(images) + ",\n";
		m += "    \"audio\": " + JNum(audio) + ",\n";
		m += "    \"models\": " + JNum(models) + ",\n";
		m += "    \"fonts\": " + JNum(fonts) + ",\n";
		m += "    \"raw\": " + JNum(raw) + ",\n";
		m += "    \"rco\": " + JNum(rcos);
		if (have_text)
			m += ",\n    \"text\": " + JNum(texts);
		m += "\n  },\n  \"files\": [\n";
		for (size_t i = 0; i < files.size(); i++) {
			m += "    {\n";
			const auto &f = files[i].fields;
			for (size_t k = 0; k < f.size(); k++) {
				m += "      " + JStr(f[k].first) + ": " + f[k].second;
				m += k + 1 < f.size() ? ",\n" : "\n";
			}
			m += i + 1 < files.size() ? "    },\n" : "    }\n";
		}
		m += "  ]\n}\n";
		WriteAll(out / "manifest.json", reinterpret_cast<const uint8_t *>(m.data()), m.size());
	}
};

}  // namespace

std::string ExtractResources(const std::string &flash0_dir, const std::string &out_dir) {
	try {
		Extractor x;
		x.flash0 = flash0_dir;
		x.resource_dir = x.flash0 / "vsh" / "resource";
		x.out = out_dir;
		std::error_code ec;
		fs::create_directories(x.out, ec);
		x.Run();
		return "";
	} catch (const std::exception &e) {
		return e.what();
	} catch (...) {
		return "unknown error while extracting resources";
	}
}

}  // namespace xmb

#ifndef XMB_EXTRACT_STANDALONE
extern "C" JNIEXPORT jstring JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeExtractResources(JNIEnv *env, jclass, jstring flash0_dir, jstring out_dir) {
	if (flash0_dir == nullptr || out_dir == nullptr)
		return env->NewStringUTF("missing path");
	const char *f = env->GetStringUTFChars(flash0_dir, nullptr);
	const char *o = env->GetStringUTFChars(out_dir, nullptr);
	std::string error = xmb::ExtractResources(f ? f : "", o ? o : "");
	if (f) env->ReleaseStringUTFChars(flash0_dir, f);
	if (o) env->ReleaseStringUTFChars(out_dir, o);
	// Error text is plain ASCII/UTF-8 from file names; sanitise for modified UTF-8.
	std::string safe;
	for (char c : error)
		safe.push_back(((uint8_t)c >= 0x80 || c == 0) ? '?' : c);
	return env->NewStringUTF(safe.c_str());
}
#endif

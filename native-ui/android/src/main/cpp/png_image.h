#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xmb {

// Decodes the 8-bit RGBA, non-interlaced PNGs produced by the resource
// preparation tools. Returns false for anything else rather than guessing.
bool DecodePng(const std::vector<uint8_t> &file, int *width, int *height,
               std::vector<uint8_t> *rgba);

bool ReadFile(const std::string &path, std::vector<uint8_t> *data);

}  // namespace xmb

// SPDX-FileCopyrightText: 2026 Pavel Švec
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>

namespace xmb {

enum class RcoError : uint16_t {
    Ok = 0,
    FileNotFound,
    FileReadFailed,
    InvalidPrfMagic,
    GmoNotFound,
    VertexChunkNotFound,
    TooFewControlPoints,
    BufferOverrun,
    CacheReadFailed,
    CacheWriteFailed,
    CacheStale,
};

const char *rcoErrorString(RcoError e);

template<typename T>
struct RcoResult {
    T value{};
    RcoError error = RcoError::Ok;

    explicit operator bool() const { return error == RcoError::Ok; }
};

struct RcoLoadReport {
    RcoError error = RcoError::Ok;
    bool hasMesh = false;
    bool hasAnim = false;
    bool hasTexture = false;
    bool cacheHit = false;
    int64_t parseMs = 0;
    int controlPointCount = 0;
    std::string parseLog;
    std::string textureMethod;
    std::string extractionMethod;
    std::string modelLabel;
    std::string imageLabel;
    float textureAvg = 0.f;
    bool textureInverted = false;
};

} // namespace xmb

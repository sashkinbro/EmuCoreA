// SPDX-FileCopyrightText: 2026 Pavel Švec
// SPDX-License-Identifier: GPL-2.0-or-later

#include "rco_error.hpp"

namespace xmb {

const char *rcoErrorString(RcoError e)
{
    switch (e) {
    case RcoError::Ok:
        return "OK";
    case RcoError::FileNotFound:
        return "File not found";
    case RcoError::FileReadFailed:
        return "File read failed";
    case RcoError::InvalidPrfMagic:
        return "Invalid PRF magic";
    case RcoError::GmoNotFound:
        return "GMO not found";
    case RcoError::VertexChunkNotFound:
        return "Vertex chunk not found";
    case RcoError::TooFewControlPoints:
        return "Too few control points";
    case RcoError::BufferOverrun:
        return "Buffer overrun";
    case RcoError::CacheReadFailed:
        return "Cache read failed";
    case RcoError::CacheWriteFailed:
        return "Cache write failed";
    case RcoError::CacheStale:
        return "Cache stale";
    }
    return "Unknown error";
}

} // namespace xmb

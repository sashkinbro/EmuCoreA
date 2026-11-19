// SPDX-FileCopyrightText: 2026 Pavel Švec
// SPDX-License-Identifier: GPL-2.0-or-later

#include "psp_wave.hpp"

#include "rco/rco_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace xmb {
namespace {

// --- GMO chunk types -------------------------------------------------------------------------
constexpr uint16_t kGmoFile = 0x0002;
constexpr uint16_t kGmoModel = 0x0003;
constexpr uint16_t kGmoBone = 0x0004;
constexpr uint16_t kGmoPart = 0x0005;
constexpr uint16_t kGmoMesh = 0x0006;
constexpr uint16_t kGmoArrays = 0x0007;
constexpr uint16_t kGmoMaterial = 0x0008;
constexpr uint16_t kGmoLayer = 0x0009;
constexpr uint16_t kGmoTexture = 0x000a;
constexpr uint16_t kGmoMotion = 0x000b;
constexpr uint16_t kGmoFCurve = 0x000c;

constexpr uint16_t kCmdMorphWeights = 0x8043;
constexpr uint16_t kCmdBoneMatrix = 0x8047;
constexpr uint16_t kCmdKnotsU = 0x8064;
constexpr uint16_t kCmdKnotsV = 0x8065;
constexpr uint16_t kCmdDrawSpline = 0x8068;
constexpr uint16_t kCmdDiffuse = 0x8082;
constexpr uint16_t kCmdAlpha = 0x8086;
constexpr uint16_t kCmdBlendFunc = 0x8094;
constexpr uint16_t kCmdFileImage = 0x8013;
constexpr uint16_t kCmdFrameLoop = 0x80b1;
constexpr uint16_t kCmdFrameRate = 0x80b2;
constexpr uint16_t kCmdAnimate = 0x80b3;

constexpr int kAnimBoneMatrix = 0x47;
constexpr int kAnimMorphWeights = 0x43;

struct Reader {
    const std::vector<uint8_t> &d;
    bool has(size_t off, size_t n) const { return off <= d.size() && n <= d.size() - off; }
    uint16_t u16(size_t off) const
    {
        uint16_t v = 0;
        if (has(off, 2))
            std::memcpy(&v, d.data() + off, 2);
        return v;
    }
    uint32_t u32(size_t off) const
    {
        uint32_t v = 0;
        if (has(off, 4))
            std::memcpy(&v, d.data() + off, 4);
        return v;
    }
    float f32(size_t off) const
    {
        float v = 0;
        if (has(off, 4))
            std::memcpy(&v, d.data() + off, 4);
        return std::isfinite(v) ? v : 0.f;
    }
};

struct RawArrays {
    uint32_t format = 0;
    uint32_t vertexCount = 0;
    int morphCount = 1;
    size_t dataOffset = 0;
    size_t dataSize = 0;
};

struct RawSpline {
    uint32_t arraysRef = 0;
    int uCount = 0;
    int vCount = 0;
    std::vector<uint16_t> indices;
};

struct RawFCurve {
    int format = 0;
    int dims = 0;
    int keys = 0;
    size_t dataOffset = 0;
    size_t dataSize = 0;
};

struct RawAnimate {
    uint32_t target = 0;
    int command = 0;
    int index = 0;
    uint32_t fcurveRef = 0;
};

struct GmoScan {
    std::vector<RawArrays> arrays;
    std::vector<RawSpline> splines;
    std::vector<float> knotsU;
    std::vector<float> knotsV;
    std::vector<RawFCurve> fcurves;
    std::vector<RawAnimate> animates;
    std::vector<float> morphWeights;
    float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool hasMatrix = false;
    float diffuse[4] = {0.588f, 0.588f, 0.588f, 1.f};
    float alpha = 1.f;
    int blend[3] = {0, 6, 1};
    size_t imageOffset = 0;
    size_t imageSize = 0;
    float frameLoop[2] = {0.f, 0.f};
    float frameRate = 30.f;
    std::string modelName;
};

std::vector<float> readKnots(const Reader &r, size_t chunkOff, uint32_t chunkSize)
{
    // Layout: 8-byte chunk header, u16 (unknown), then packed float32 knots (2-byte aligned).
    std::vector<float> knots;
    if (chunkSize < 14)
        return knots;
    const size_t count = (chunkSize - 10) / 4;
    knots.reserve(count);
    for (size_t i = 0; i < count; ++i)
        knots.push_back(r.f32(chunkOff + 10 + i * 4));
    return knots;
}

void scanChunks(const Reader &r, size_t off, size_t end, GmoScan &scan, int depth)
{
    if (depth > 12)
        return;
    while (off + 8 <= end && r.has(off, 8)) {
        const uint16_t type = r.u16(off);
        const uint16_t argsOff = r.u16(off + 2);
        const uint32_t size = r.u32(off + 4);
        if (size < 8 || off + size > end)
            return;
        const size_t args = off + 8;

        switch (type) {
        case kGmoFile:
        case kGmoModel:
        case kGmoBone:
        case kGmoPart:
        case kGmoMesh:
        case kGmoMaterial:
        case kGmoLayer:
        case kGmoTexture:
        case kGmoMotion:
            if (type == kGmoModel && scan.modelName.empty() && argsOff >= 0x18) {
                const size_t nameOff = off + 16;
                const size_t maxLen = argsOff - 16;
                std::string name;
                for (size_t i = 0; i < maxLen && r.has(nameOff + i, 1) && r.d[nameOff + i]; ++i)
                    name.push_back(char(r.d[nameOff + i]));
                scan.modelName = name;
            }
            if (argsOff >= 8)
                scanChunks(r, off + argsOff, off + size, scan, depth + 1);
            break;
        case kGmoArrays: {
            const size_t a = off + argsOff;
            RawArrays arr;
            arr.format = r.u32(a);
            arr.vertexCount = r.u32(a + 4);
            arr.morphCount = int((arr.format >> 18) & 7) + 1;
            arr.dataOffset = a + 16;
            arr.dataSize = off + size > arr.dataOffset ? off + size - arr.dataOffset : 0;
            scan.arrays.push_back(arr);
            break;
        }
        case kGmoFCurve: {
            const size_t a = off + argsOff;
            RawFCurve fc;
            fc.format = int(r.u32(a));
            fc.dims = int(r.u32(a + 4));
            fc.keys = int(r.u32(a + 8));
            fc.dataOffset = a + 16;
            fc.dataSize = off + size > fc.dataOffset ? off + size - fc.dataOffset : 0;
            scan.fcurves.push_back(fc);
            break;
        }
        case kCmdMorphWeights: {
            const uint32_t n = std::min<uint32_t>(r.u32(args), 8);
            scan.morphWeights.clear();
            for (uint32_t i = 0; i < n; ++i)
                scan.morphWeights.push_back(r.f32(args + 4 + i * 4));
            break;
        }
        case kCmdBoneMatrix:
            if (size >= 8 + 64) {
                for (int i = 0; i < 16; ++i)
                    scan.matrix[i] = r.f32(args + size_t(i) * 4);
                scan.hasMatrix = true;
            }
            break;
        case kCmdKnotsU:
            scan.knotsU = readKnots(r, off, size);
            break;
        case kCmdKnotsV:
            scan.knotsV = readKnots(r, off, size);
            break;
        case kCmdDrawSpline: {
            RawSpline sp;
            sp.arraysRef = r.u32(args);
            sp.uCount = int(r.u32(args + 8));
            sp.vCount = int(r.u32(args + 12));
            const size_t n = size_t(std::max(0, sp.uCount)) * size_t(std::max(0, sp.vCount));
            if (n > 0 && n < 65536 && r.has(args + 16, n * 2)) {
                sp.indices.resize(n);
                for (size_t i = 0; i < n; ++i)
                    sp.indices[i] = r.u16(args + 16 + i * 2);
                scan.splines.push_back(std::move(sp));
            }
            break;
        }
        case kCmdDiffuse:
            for (int i = 0; i < 4; ++i)
                scan.diffuse[i] = r.f32(args + size_t(i) * 4);
            break;
        case kCmdAlpha:
            scan.alpha = r.f32(args);
            break;
        case kCmdBlendFunc:
            for (int i = 0; i < 3; ++i)
                scan.blend[i] = int(r.u32(args + size_t(i) * 4));
            break;
        case kCmdFileImage:
            if (scan.imageSize == 0 && size > 12) {
                scan.imageOffset = args + 4;
                scan.imageSize = std::min<size_t>(r.u32(args), size - 12);
            }
            break;
        case kCmdFrameLoop:
            scan.frameLoop[0] = r.f32(args);
            scan.frameLoop[1] = r.f32(args + 4);
            break;
        case kCmdFrameRate:
            scan.frameRate = r.f32(args);
            break;
        case kCmdAnimate: {
            RawAnimate an;
            an.target = r.u32(args);
            an.command = int(r.u32(args + 4));
            an.index = int(r.u32(args + 8));
            an.fcurveRef = r.u32(args + 12);
            scan.animates.push_back(an);
            break;
        }
        default:
            break;
        }
        off += size;
    }
}

// Byte size / alignment of one GE vertex (for a single morph target).
bool geVertexLayout(uint32_t fmt, size_t &stride, size_t &posOffset, int &posType)
{
    size_t off = 0;
    size_t maxAlign = 1;
    auto place = [&](size_t size, size_t align) {
        off = (off + align - 1) & ~(align - 1);
        const size_t at = off;
        off += size;
        maxAlign = std::max(maxAlign, align);
        return at;
    };

    const int wt = int((fmt >> 9) & 3);
    const int wc = int((fmt >> 14) & 7) + 1;
    if (wt) {
        const size_t s = wt == 1 ? 1 : wt == 2 ? 2 : 4;
        place(s * size_t(wc), s);
    }
    const int tt = int(fmt & 3);
    if (tt) {
        const size_t s = tt == 1 ? 1 : tt == 2 ? 2 : 4;
        place(s * 2, s);
    }
    const int ct = int((fmt >> 2) & 7);
    if (ct >= 4 && ct <= 6)
        place(2, 2);
    else if (ct == 7)
        place(4, 4);
    const int nt = int((fmt >> 5) & 3);
    if (nt) {
        const size_t s = nt == 1 ? 1 : nt == 2 ? 2 : 4;
        place(s * 3, s);
    }
    posType = int((fmt >> 7) & 3);
    if (!posType)
        return false;
    const size_t ps = posType == 1 ? 1 : posType == 2 ? 2 : 4;
    posOffset = place(ps * 3, ps);
    stride = (off + maxAlign - 1) & ~(maxAlign - 1);
    return stride > 0;
}

bool buildTrack(const Reader &r, const RawFCurve &fc, PspWaveTrack &track, std::string &log)
{
    if (fc.dims <= 0 || fc.dims > 64 || fc.keys <= 0 || fc.keys > 100000)
        return false;
    const size_t perKey = fc.dataSize / size_t(fc.keys) / 4; // floats per key
    if (perKey < size_t(1 + fc.dims))
        return false;
    track.dims = fc.dims;
    track.times.resize(size_t(fc.keys));
    track.values.resize(size_t(fc.keys) * size_t(fc.dims));
    for (int k = 0; k < fc.keys; ++k) {
        const size_t base = fc.dataOffset + size_t(k) * perKey * 4;
        track.times[size_t(k)] = r.f32(base);
        for (int c = 0; c < fc.dims; ++c)
            track.values[size_t(k) * size_t(fc.dims) + size_t(c)] = r.f32(base + 4 + size_t(c) * 4);
    }
    if (perKey != size_t(1 + fc.dims)) {
        std::ostringstream s;
        s << "fcurve format " << fc.format << " stride " << perKey << " (values only); ";
        log += s.str();
    }
    return true;
}

std::vector<float> clampedUniformKnots(int count, int degree)
{
    std::vector<float> k;
    const int n = count + degree + 1;
    const int interior = count - degree;
    for (int i = 0; i < n; ++i) {
        if (i <= degree)
            k.push_back(0.f);
        else if (i >= count)
            k.push_back(1.f);
        else
            k.push_back(float(i - degree) / float(interior));
    }
    return k;
}

bool knotsUsable(const std::vector<float> &k, int count)
{
    if (int(k.size()) < count + 2 || int(k.size()) > count + 4)
        return false;
    for (size_t i = 1; i < k.size(); ++i) {
        if (!(k[i] >= k[i - 1]))
            return false;
    }
    return k.back() > k.front();
}

int findSpan(const std::vector<float> &U, int n, int p, float u)
{
    // n = count - 1
    if (u >= U[size_t(n + 1)])
        return n;
    if (u <= U[size_t(p)])
        return p;
    int low = p;
    int high = n + 1;
    int mid = (low + high) / 2;
    while (u < U[size_t(mid)] || u >= U[size_t(mid + 1)]) {
        if (u < U[size_t(mid)])
            high = mid;
        else
            low = mid;
        mid = (low + high) / 2;
    }
    return mid;
}

} // namespace

// --- Track ---------------------------------------------------------------------------------------

void PspWaveTrack::evaluate(float frame, float *out) const
{
    if (!valid())
        return;
    const size_t n = times.size();
    const size_t D = size_t(dims);
    if (n == 1 || frame <= times.front()) {
        std::copy_n(values.begin(), D, out);
        return;
    }
    if (frame >= times.back()) {
        std::copy_n(values.begin() + std::ptrdiff_t((n - 1) * D), D, out);
        return;
    }
    const auto it = std::upper_bound(times.begin(), times.end(), frame);
    const size_t i1 = size_t(it - times.begin());
    const size_t i0 = i1 - 1;
    const float t0 = times[i0];
    const float t1 = times[i1];
    const float f = t1 > t0 ? (frame - t0) / (t1 - t0) : 0.f;
    for (size_t c = 0; c < D; ++c)
        out[c] = values[i0 * D + c] + (values[i1 * D + c] - values[i0 * D + c]) * f;
}

// --- TGA -----------------------------------------------------------------------------------------

bool decodeTga(const uint8_t *data, size_t size, std::vector<uint8_t> &rgba, int &w, int &h)
{
    if (!data || size < 18)
        return false;
    const uint8_t idLen = data[0];
    const uint8_t cmapType = data[1];
    const uint8_t imgType = data[2];
    const uint16_t cmapFirst = uint16_t(data[3] | (data[4] << 8));
    const uint16_t cmapLen = uint16_t(data[5] | (data[6] << 8));
    const uint8_t cmapBits = data[7];
    w = data[12] | (data[13] << 8);
    h = data[14] | (data[15] << 8);
    const uint8_t bpp = data[16];
    const uint8_t desc = data[17];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return false;
    const bool rle = imgType >= 9;
    const int base = rle ? imgType - 8 : imgType;
    if (base < 1 || base > 3)
        return false;

    size_t pos = 18 + idLen;
    std::vector<uint8_t> palette; // RGBA
    if (cmapType == 1) {
        const size_t eb = (cmapBits + 7) / 8;
        if (pos + eb * cmapLen > size)
            return false;
        palette.resize(size_t(cmapFirst + cmapLen) * 4, 0);
        for (size_t i = 0; i < cmapLen; ++i) {
            const uint8_t *p = data + pos + i * eb;
            uint8_t *o = palette.data() + (cmapFirst + i) * 4;
            if (eb >= 3) {
                o[0] = p[2];
                o[1] = p[1];
                o[2] = p[0];
                o[3] = eb == 4 ? p[3] : 255;
            } else if (eb == 2) {
                const uint16_t v = uint16_t(p[0] | (p[1] << 8));
                o[0] = uint8_t(((v >> 10) & 31) * 255 / 31);
                o[1] = uint8_t(((v >> 5) & 31) * 255 / 31);
                o[2] = uint8_t((v & 31) * 255 / 31);
                o[3] = 255;
            }
        }
        pos += eb * cmapLen;
    }

    const size_t pb = (bpp + 7) / 8;
    if (pb == 0 || pb > 4)
        return false;
    const size_t count = size_t(w) * size_t(h);
    std::vector<uint8_t> pixels(count * pb);
    if (!rle) {
        if (pos + pixels.size() > size)
            return false;
        std::memcpy(pixels.data(), data + pos, pixels.size());
    } else {
        size_t out = 0;
        while (out < count && pos < size) {
            const uint8_t hdr = data[pos++];
            const size_t run = (hdr & 0x7f) + 1;
            if (hdr & 0x80) {
                if (pos + pb > size)
                    return false;
                for (size_t i = 0; i < run && out < count; ++i, ++out)
                    std::memcpy(pixels.data() + out * pb, data + pos, pb);
                pos += pb;
            } else {
                if (pos + run * pb > size)
                    return false;
                const size_t n = std::min(run, count - out);
                std::memcpy(pixels.data() + out * pb, data + pos, n * pb);
                pos += run * pb;
                out += n;
            }
        }
    }

    rgba.assign(count * 4, 0);
    const bool topDown = (desc & 0x20) != 0;
    for (int y = 0; y < h; ++y) {
        const int srcY = topDown ? y : h - 1 - y;
        for (int x = 0; x < w; ++x) {
            const uint8_t *p = pixels.data() + (size_t(srcY) * size_t(w) + size_t(x)) * pb;
            uint8_t *o = rgba.data() + (size_t(y) * size_t(w) + size_t(x)) * 4;
            if (base == 1) {
                const size_t idx = pb == 1 ? p[0] : size_t(p[0] | (p[1] << 8));
                if (idx * 4 + 3 < palette.size())
                    std::memcpy(o, palette.data() + idx * 4, 4);
            } else if (base == 2) {
                if (pb >= 3) {
                    o[0] = p[2];
                    o[1] = p[1];
                    o[2] = p[0];
                    o[3] = pb == 4 ? p[3] : 255;
                } else {
                    const uint16_t v = uint16_t(p[0] | (p[1] << 8));
                    o[0] = uint8_t(((v >> 10) & 31) * 255 / 31);
                    o[1] = uint8_t(((v >> 5) & 31) * 255 / 31);
                    o[2] = uint8_t((v & 31) * 255 / 31);
                    o[3] = 255;
                }
            } else {
                o[0] = o[1] = o[2] = p[0];
                o[3] = 255;
            }
        }
    }
    return true;
}

// --- GMO -----------------------------------------------------------------------------------------

PspWaveLoadResult parsePspWaveGmo(const std::vector<uint8_t> &gmo, PspWave &out)
{
    PspWaveLoadResult res;
    if (gmo.size() < 32 || std::memcmp(gmo.data(), "OMG.00.1PSP", 11) != 0) {
        res.error = "not a GMO file";
        return res;
    }

    const Reader r{gmo};
    GmoScan scan;
    scanChunks(r, 16, gmo.size(), scan, 0);
    std::ostringstream log;

    if (scan.splines.empty() || scan.arrays.empty()) {
        res.error = "GMO has no spline surface";
        return res;
    }
    const RawSpline &sp = scan.splines.front();
    const size_t arrIdx = std::min<size_t>(sp.arraysRef & 0xfff, scan.arrays.size() - 1);
    const RawArrays &arr = scan.arrays[arrIdx];

    size_t stride = 0, posOff = 0;
    int posType = 0;
    if (!geVertexLayout(arr.format, stride, posOff, posType)) {
        res.error = "unsupported vertex format";
        return res;
    }
    const size_t vertexBytes = stride * size_t(arr.morphCount);
    if (arr.vertexCount == 0 || vertexBytes * arr.vertexCount > arr.dataSize) {
        res.error = "vertex array truncated";
        return res;
    }

    // Some modded RCOs carry corrupted spline indices; the vertex array is always stored in
    // grid order, so fall back to sequential indices when an index is out of range.
    std::vector<uint16_t> indices = sp.indices;
    const bool badIndex = std::any_of(indices.begin(), indices.end(),
                                      [&](uint16_t i) { return i >= arr.vertexCount; });
    if (badIndex) {
        if (indices.size() > arr.vertexCount) {
            res.error = "spline index out of range";
            return res;
        }
        for (size_t i = 0; i < indices.size(); ++i)
            indices[i] = uint16_t(i);
        log << "indices: corrupted, using grid order; ";
    }

    out.uCount = sp.uCount;
    out.vCount = sp.vCount;
    out.morphCount = arr.morphCount;
    out.ctrl.assign(size_t(out.morphCount) * size_t(out.uCount) * size_t(out.vCount) * 3, 0.f);
    const float posScale = posType == 1 ? 1.f / 128.f : posType == 2 ? 1.f / 32768.f : 1.f;
    for (int v = 0; v < out.vCount; ++v) {
        for (int u = 0; u < out.uCount; ++u) {
            const uint16_t vi = indices[size_t(v * out.uCount + u)];
            for (int m = 0; m < out.morphCount; ++m) {
                const size_t base = arr.dataOffset + size_t(vi) * vertexBytes + size_t(m) * stride + posOff;
                float *dst = &out.ctrl[((size_t(m) * size_t(out.vCount) + size_t(v)) * size_t(out.uCount) + size_t(u)) * 3];
                for (int c = 0; c < 3; ++c) {
                    if (posType == 3) {
                        dst[c] = r.f32(base + size_t(c) * 4);
                    } else if (posType == 2) {
                        dst[c] = float(int16_t(r.u16(base + size_t(c) * 2))) * posScale;
                    } else {
                        dst[c] = r.has(base + size_t(c), 1) ? float(int8_t(gmo[base + size_t(c)])) * posScale : 0.f;
                    }
                }
            }
        }
    }

    out.knotsU = knotsUsable(scan.knotsU, out.uCount) ? scan.knotsU : clampedUniformKnots(out.uCount, 3);
    out.knotsV = knotsUsable(scan.knotsV, out.vCount) ? scan.knotsV : clampedUniformKnots(out.vCount, 3);
    if (!knotsUsable(scan.knotsU, out.uCount) || !knotsUsable(scan.knotsV, out.vCount))
        log << "knots: fallback uniform; ";

    if (scan.hasMatrix)
        std::copy_n(scan.matrix, 16, out.restMatrix);
    out.restMorph.assign(size_t(out.morphCount), 0.f);
    if (!scan.morphWeights.empty()) {
        for (size_t i = 0; i < out.restMorph.size() && i < scan.morphWeights.size(); ++i)
            out.restMorph[i] = scan.morphWeights[i];
    } else {
        out.restMorph[0] = 1.f;
    }

    for (const RawAnimate &an : scan.animates) {
        const size_t fi = an.fcurveRef & 0xfff;
        if (fi >= scan.fcurves.size())
            continue;
        if (an.command == kAnimBoneMatrix) {
            PspWaveTrack t;
            if (buildTrack(r, scan.fcurves[fi], t, out.log) && t.dims == 16)
                out.matrixTrack = std::move(t);
        } else if (an.command == kAnimMorphWeights) {
            PspWaveTrack t;
            if (buildTrack(r, scan.fcurves[fi], t, out.log) && t.dims >= 1)
                out.morphTrack = std::move(t);
        } else {
            log << "anim cmd 0x" << std::hex << an.command << std::dec << " ignored; ";
        }
    }

    if (scan.frameLoop[1] > scan.frameLoop[0]) {
        out.frameStart = scan.frameLoop[0];
        out.frameEnd = scan.frameLoop[1];
    } else {
        float end = 0.f;
        if (out.matrixTrack.valid())
            end = std::max(end, out.matrixTrack.times.back());
        if (out.morphTrack.valid())
            end = std::max(end, out.morphTrack.times.back());
        out.frameStart = 0.f;
        out.frameEnd = end;
    }
    out.frameRate = scan.frameRate > 1.f && scan.frameRate < 240.f ? scan.frameRate : 30.f;

    std::copy_n(scan.diffuse, 4, out.diffuse);
    out.alpha = scan.alpha > 0.f && scan.alpha <= 1.f ? scan.alpha : 1.f;
    out.blendMode = scan.blend[0];
    out.blendSrc = scan.blend[1];
    out.blendDst = scan.blend[2];
    out.modelName = scan.modelName;

    out.rgba.clear();
    out.texW = out.texH = 0;
    if (scan.imageSize > 18 && r.has(scan.imageOffset, scan.imageSize)) {
        if (!decodeTga(gmo.data() + scan.imageOffset, scan.imageSize, out.rgba, out.texW, out.texH)) {
            out.rgba.clear();
            out.texW = out.texH = 0;
            log << "texture: unsupported image; ";
        }
    }

    log << "grid " << out.uCount << "x" << out.vCount << " morphs " << out.morphCount << " knots "
        << out.knotsU.size() << "/" << out.knotsV.size() << " frames " << out.frameStart << "-" << out.frameEnd
        << " @" << out.frameRate << " tex " << out.texW << "x" << out.texH << " tracks m"
        << out.matrixTrack.times.size() << " w" << out.morphTrack.times.size();
    out.log += log.str();
    res.ok = true;
    return res;
}

PspWaveLoadResult loadPspWaveRco(const std::string &rcoPath, PspWave &out)
{
    PspWaveLoadResult res;
    std::ifstream f(rcoPath, std::ios::binary);
    if (!f) {
        res.error = "cannot open " + rcoPath;
        return res;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty() || data.size() > (64u << 20)) {
        res.error = "invalid RCO size";
        return res;
    }

    const auto doc = rco::readRcoDocument(data);
    if (doc.error != RcoError::Ok) {
        res.error = std::string("RCO: ") + rcoErrorString(doc.error);
        return res;
    }
    const rco::WaveScene &scene = doc.value.scene;
    std::vector<const rco::RcoNode *> candidates;
    if (scene.modelEntry)
        candidates.push_back(scene.modelEntry);
    for (const rco::RcoNode *m : doc.value.models) {
        if (m != scene.modelEntry)
            candidates.push_back(m);
    }
    if (candidates.empty()) {
        res.error = "RCO contains no model";
        return res;
    }

    for (const rco::RcoNode *m : candidates) {
        const auto blob = rco::readNodeResource(doc.value, *m);
        if (blob.error != RcoError::Ok)
            continue;
        PspWave wave;
        const auto r = parsePspWaveGmo(blob.value, wave);
        if (!r.ok) {
            res.error = r.error;
            continue;
        }
        if (scene.foundModelObject) {
            const auto &t = scene.modelObject;
            wave.modelPos[0] = t.posX;
            wave.modelPos[1] = t.posY;
            wave.modelPos[2] = t.posZ;
            const auto sane = [](float s) { return std::isfinite(s) && std::abs(s) > 1e-3f && std::abs(s) < 1e3f; };
            if (sane(t.scaleWidth) && sane(t.scaleHeight) && sane(t.scaleDepth)) {
                wave.modelScale[0] = t.scaleWidth;
                wave.modelScale[1] = t.scaleHeight;
                wave.modelScale[2] = t.scaleDepth;
            }
            const float tint[4] = {t.redScale, t.greenScale, t.blueScale, t.alphaScale};
            for (int i = 0; i < 4; ++i)
                wave.tint[i] = std::isfinite(tint[i]) ? std::clamp(tint[i], 0.f, 4.f) : 1.f;
        }
        wave.log = "model " + m->label + ": " + wave.log;
        out = std::move(wave);
        res.ok = true;
        res.error.clear();
        return res;
    }
    if (res.error.empty())
        res.error = "no usable model resource";
    return res;
}

// --- Pose / tessellation -------------------------------------------------------------------------

void pspWavePoseAt(const PspWave &wave, float frame, float *m, std::vector<float> &weights)
{
    std::copy_n(wave.restMatrix, 16, m);
    if (wave.matrixTrack.valid())
        wave.matrixTrack.evaluate(frame, m);

    weights = wave.restMorph;
    if (wave.morphTrack.valid()) {
        std::vector<float> tmp(size_t(wave.morphTrack.dims), 0.f);
        wave.morphTrack.evaluate(frame, tmp.data());
        for (size_t i = 0; i < weights.size() && i < tmp.size(); ++i)
            weights[i] = tmp[i];
    }

    // Keep the linear-interpolated rotation part orthonormal (sparse keyframes shrink it).
    auto col = [&](int c) { return m + c * 4; };
    for (int c = 0; c < 3; ++c) {
        float *v = col(c);
        for (int p = 0; p < c; ++p) {
            const float *q = col(p);
            const float d = v[0] * q[0] + v[1] * q[1] + v[2] * q[2];
            v[0] -= d * q[0];
            v[1] -= d * q[1];
            v[2] -= d * q[2];
        }
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 1e-6f) {
            v[0] /= len;
            v[1] /= len;
            v[2] /= len;
        }
    }
}

std::vector<PspWaveTessellator::Basis> PspWaveTessellator::buildBasis(const std::vector<float> &U, int count, int samples)
{
    std::vector<Basis> out(static_cast<size_t>(samples));
    const int p = std::clamp(int(U.size()) - count - 1, 1, 3);
    const int n = count - 1;
    const float u0 = U[size_t(p)];
    const float u1 = U[size_t(n + 1)];

    for (int s = 0; s < samples; ++s) {
        const float u = u0 + (u1 - u0) * float(s) / float(samples - 1);
        const int span = findSpan(U, n, p, u);

        // NURBS book A2.3 (first derivative only).
        float ndu[4][4] = {};
        float left[4] = {}, right[4] = {};
        ndu[0][0] = 1.f;
        for (int j = 1; j <= p; ++j) {
            left[j] = u - U[size_t(span + 1 - j)];
            right[j] = U[size_t(span + j)] - u;
            float saved = 0.f;
            for (int r = 0; r < j; ++r) {
                ndu[j][r] = right[r + 1] + left[j - r];
                const float temp = ndu[j][r] != 0.f ? ndu[r][j - 1] / ndu[j][r] : 0.f;
                ndu[r][j] = saved + right[r + 1] * temp;
                saved = left[j - r] * temp;
            }
            ndu[j][j] = saved;
        }
        Basis b;
        b.span = span - p;
        for (int j = 0; j <= p; ++j)
            b.n[j] = ndu[j][p];
        for (int r = 0; r <= p; ++r) {
            float d = 0.f;
            if (r >= 1) {
                const float den = ndu[p][r - 1];
                if (den != 0.f)
                    d += ndu[r - 1][p - 1] / den;
            }
            if (r <= p - 1) {
                const float den = ndu[p][r];
                if (den != 0.f)
                    d -= ndu[r][p - 1] / den;
            }
            b.d[r] = d * float(p);
        }
        out[size_t(s)] = b;
    }
    return out;
}

void PspWaveTessellator::setup(const PspWave &wave, int uSegments, int vSegments)
{
    m_uSamples = m_vSamples = 0;
    m_indices.clear();
    m_lineIndices.clear();
    if (wave.uCount < 2 || wave.vCount < 2)
        return;
    m_uCount = wave.uCount;
    m_vCount = wave.vCount;
    m_uSamples = std::max(4, uSegments + 1);
    m_vSamples = std::max(4, vSegments + 1);
    m_bu = buildBasis(wave.knotsU, wave.uCount, m_uSamples);
    m_bv = buildBasis(wave.knotsV, wave.vCount, m_vSamples);

    m_indices.reserve(size_t(m_uSamples - 1) * size_t(m_vSamples - 1) * 6);
    for (int v = 0; v + 1 < m_vSamples; ++v) {
        for (int u = 0; u + 1 < m_uSamples; ++u) {
            const uint32_t a = uint32_t(v * m_uSamples + u);
            const uint32_t b = a + 1;
            const uint32_t c = a + uint32_t(m_uSamples);
            const uint32_t d = c + 1;
            m_indices.insert(m_indices.end(), {a, c, b, b, c, d});
        }
    }
    for (int v = 0; v < m_vSamples; v += std::max(1, m_vSamples / 12)) {
        for (int u = 0; u + 1 < m_uSamples; ++u)
            m_lineIndices.insert(m_lineIndices.end(), {uint32_t(v * m_uSamples + u), uint32_t(v * m_uSamples + u + 1)});
    }
    for (int u = 0; u < m_uSamples; u += std::max(1, m_uSamples / 24)) {
        for (int v = 0; v + 1 < m_vSamples; ++v)
            m_lineIndices.insert(m_lineIndices.end(), {uint32_t(v * m_uSamples + u), uint32_t((v + 1) * m_uSamples + u)});
    }
}

void PspWaveTessellator::evaluate(const PspWave &wave, float frame, std::vector<float> &vertices) const
{
    if (!valid() || wave.uCount != m_uCount || wave.vCount != m_vCount)
        return;

    float M[16];
    std::vector<float> w;
    pspWavePoseAt(wave, frame, M, w);

    // Blend morph targets and apply bone matrix + ModelObject transform to the control points
    // (B-splines are affine invariant, so posing the hull poses the surface).
    const size_t cps = size_t(m_uCount) * size_t(m_vCount);
    m_blended.assign(cps * 3, 0.f);
    for (int m = 0; m < wave.morphCount && m < int(w.size()); ++m) {
        const float wm = w[size_t(m)];
        if (wm == 0.f)
            continue;
        const float *src = wave.ctrl.data() + size_t(m) * cps * 3;
        for (size_t i = 0; i < cps * 3; ++i)
            m_blended[i] += src[i] * wm;
    }
    for (size_t i = 0; i < cps; ++i) {
        float *p = &m_blended[i * 3];
        const float x = p[0], y = p[1], z = p[2];
        const float tx = M[0] * x + M[4] * y + M[8] * z + M[12];
        const float ty = M[1] * x + M[5] * y + M[9] * z + M[13];
        const float tz = M[2] * x + M[6] * y + M[10] * z + M[14];
        p[0] = tx * wave.modelScale[0] + wave.modelPos[0];
        p[1] = ty * wave.modelScale[1] + wave.modelPos[1];
        p[2] = tz * wave.modelScale[2] + wave.modelPos[2];
    }

    // Separable evaluation: first along u for every control row (value + u-derivative),
    // then along v. 4 + 4 control-point taps per vertex instead of 16.
    const size_t U = size_t(m_uSamples);
    m_rowP.resize(size_t(m_vCount) * U * 3);
    m_rowDu.resize(size_t(m_vCount) * U * 3);
    for (int row = 0; row < m_vCount; ++row) {
        const float *crow = &m_blended[size_t(row) * size_t(m_uCount) * 3];
        float *rp = &m_rowP[size_t(row) * U * 3];
        float *rd = &m_rowDu[size_t(row) * U * 3];
        for (size_t u = 0; u < U; ++u) {
            const Basis &bu = m_bu[u];
            float P[3] = {}, D[3] = {};
            for (int i = 0; i < 4; ++i) {
                const int ci = bu.span + i;
                if (ci < 0 || ci >= m_uCount)
                    continue;
                const float *c = crow + size_t(ci) * 3;
                for (int k = 0; k < 3; ++k) {
                    P[k] += c[k] * bu.n[i];
                    D[k] += c[k] * bu.d[i];
                }
            }
            std::copy_n(P, 3, rp + u * 3);
            std::copy_n(D, 3, rd + u * 3);
        }
    }

    vertices.resize(U * size_t(m_vSamples) * 6);
    float *out = vertices.data();
    for (int v = 0; v < m_vSamples; ++v) {
        const Basis &bv = m_bv[size_t(v)];
        const float *rowsP[4] = {};
        const float *rowsD[4] = {};
        for (int j = 0; j < 4; ++j) {
            const int row = bv.span + j;
            if (row >= 0 && row < m_vCount) {
                rowsP[j] = &m_rowP[size_t(row) * U * 3];
                rowsD[j] = &m_rowDu[size_t(row) * U * 3];
            }
        }
        for (size_t u = 0; u < U; ++u) {
            float P[3] = {}, Du[3] = {}, Dv[3] = {};
            for (int j = 0; j < 4; ++j) {
                if (!rowsP[j])
                    continue;
                const float *rp = rowsP[j] + u * 3;
                const float *rd = rowsD[j] + u * 3;
                for (int k = 0; k < 3; ++k) {
                    P[k] += rp[k] * bv.n[j];
                    Du[k] += rd[k] * bv.n[j];
                    Dv[k] += rp[k] * bv.d[j];
                }
            }
            float N[3] = {Du[1] * Dv[2] - Du[2] * Dv[1], Du[2] * Dv[0] - Du[0] * Dv[2], Du[0] * Dv[1] - Du[1] * Dv[0]};
            const float len = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
            if (len > 1e-8f) {
                const float inv = 1.f / len;
                N[0] *= inv;
                N[1] *= inv;
                N[2] *= inv;
            } else {
                N[0] = 0.f;
                N[1] = 0.f;
                N[2] = 1.f;
            }
            out[0] = P[0];
            out[1] = P[1];
            out[2] = P[2];
            out[3] = N[0];
            out[4] = N[1];
            out[5] = N[2];
            out += 6;
        }
    }
}

} // namespace xmb

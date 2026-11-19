// SPDX-FileCopyrightText: 2026 Pavel Švec
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Faithful model of the PSP XMB wave as stored in system_plugin_bg.rco:
//
//   RCO  -> ModelObject (position / scale) + model resource "mdl_bg" (GMO)
//   GMO  -> one bone with a 4x4 matrix and morph weights,
//           one mesh drawn as a cubic B-spline surface (DrawSpline, u x v control points,
//           non-uniform knot vectors), vertex array with N interleaved morph targets,
//           material (diffuse, alpha, blend func) with a sphere/reflection texture (TGA),
//           motion: linear fcurves animating the bone matrix and the morph weights.
//
// Everything here is plain C++ (no Qt) so it can be unit-tested and used from tools.

#include <cstdint>
#include <string>
#include <vector>

namespace xmb {

struct PspWaveTrack {
    int dims = 0;
    std::vector<float> times;  // frame numbers, ascending
    std::vector<float> values; // times.size() * dims
    bool valid() const { return dims > 0 && !times.empty() && values.size() == times.size() * size_t(dims); }
    void evaluate(float frame, float *out) const;
};

struct PspWave {
    // Control grid, u runs fastest. ctrl[(m * vCount + v) * uCount + u] = xyz for morph target m.
    int uCount = 0;
    int vCount = 0;
    int morphCount = 0;
    std::vector<float> ctrl; // morphCount * vCount * uCount * 3
    std::vector<float> knotsU;
    std::vector<float> knotsV;

    float restMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // column-major
    std::vector<float> restMorph;                                           // morphCount weights

    PspWaveTrack matrixTrack; // 16 dims, bone command 0x47
    PspWaveTrack morphTrack;  // morphCount dims, bone command 0x43

    float frameStart = 0.f;
    float frameEnd = 1500.f;
    float frameRate = 30.f;

    // Material
    float diffuse[4] = {0.588f, 0.588f, 0.588f, 1.f};
    float alpha = 0.75f;
    int blendMode = 0; // 0 = add
    int blendSrc = 6;  // SRC_ALPHA
    int blendDst = 1;  // ONE

    // Texture (RGBA8, top row first)
    std::vector<uint8_t> rgba;
    int texW = 0;
    int texH = 0;

    // RCO ModelObject transform
    float modelPos[3] = {0.f, 0.f, 0.f};
    float modelScale[3] = {8.5f, 8.5f, 8.5f};
    float tint[4] = {1.f, 1.f, 1.f, 1.f};

    std::string modelName;
    std::string log;

    float loopFrames() const { return frameEnd > frameStart ? frameEnd - frameStart : 0.f; }
};

struct PspWaveLoadResult {
    bool ok = false;
    std::string error;
};

/// Parse a GMO blob ("OMG.00.1PSP").
PspWaveLoadResult parsePspWaveGmo(const std::vector<uint8_t> &gmo, PspWave &out);

/// Load RCO file, locate the model resource + ModelObject, parse the GMO.
PspWaveLoadResult loadPspWaveRco(const std::string &rcoPath, PspWave &out);

/// Decode a TGA image (types 1, 2, 3, 9, 10, 11) into RGBA8, top row first.
bool decodeTga(const uint8_t *data, size_t size, std::vector<uint8_t> &rgba, int &w, int &h);

/// Precomputed tessellation of the B-spline surface; evaluate() produces a posed mesh.
class PspWaveTessellator {
public:
    void setup(const PspWave &wave, int uSegments, int vSegments);
    bool valid() const { return m_uSamples > 1 && m_vSamples > 1; }

    int uSamples() const { return m_uSamples; }
    int vSamples() const { return m_vSamples; }

    /// Evaluate at animation frame. Output: interleaved position (3) + normal (3) in world units
    /// (ModelObject transform applied), and triangle indices (rebuilt only on setup()).
    void evaluate(const PspWave &wave, float frame, std::vector<float> &vertices) const;

    const std::vector<uint32_t> &indices() const { return m_indices; }
    const std::vector<uint32_t> &lineIndices() const { return m_lineIndices; }

private:
    struct Basis {
        int span = 0;   // first control point index
        float n[4] = {};  // basis values
        float d[4] = {};  // derivatives
    };
    static std::vector<Basis> buildBasis(const std::vector<float> &knots, int count, int samples);

    int m_uSamples = 0;
    int m_vSamples = 0;
    int m_uCount = 0;
    int m_vCount = 0;
    std::vector<Basis> m_bu;
    std::vector<Basis> m_bv;
    std::vector<uint32_t> m_indices;
    std::vector<uint32_t> m_lineIndices;
    mutable std::vector<float> m_blended;
    mutable std::vector<float> m_rowP;
    mutable std::vector<float> m_rowDu;
};

/// Compose bone pose at frame: morph weights + matrix (column-major).
void pspWavePoseAt(const PspWave &wave, float frame, float *matrix16, std::vector<float> &weights);

} // namespace xmb

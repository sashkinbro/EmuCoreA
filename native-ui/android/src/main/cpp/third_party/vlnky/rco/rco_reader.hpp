// SPDX-FileCopyrightText: 2026 Pavel Švec
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "rco_types.hpp"
#include "../rco_error.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace xmb::rco {

struct ResourceDesc {
    uint32_t fileOffset = 0;
    uint32_t packedSize = 0;
    uint32_t unpackedSize = 0;
    uint16_t compression = 0;
    uint16_t format = 0;
    bool valid = false;
};

struct RcoNode {
    uint32_t fileOffset = 0;
    uint32_t nextRel = 0;
    int tableId = 0;
    int type = 0;
    std::string label;
    ResourceDesc resource;
    std::vector<uint8_t> extra;
    std::vector<RcoNode> children;
};

struct WaveSceneTransform {
    float posX = 0, posY = 0, posZ = 0;
    float redScale = 1, greenScale = 1, blueScale = 1, alphaScale = 1;
    float width = 0, height = 0, depth = 0;
    float scaleWidth = 1, scaleHeight = 1, scaleDepth = 1;
};

struct WaveScene {
    std::string modelLabel = "mdl_bg";
    WaveSceneTransform modelObject;
    bool foundModelObject = false;
    const RcoNode *modelEntry = nullptr;
    const RcoNode *imageEntry = nullptr;
};

struct RcoDocument {
    PrfHeader header{};
    uint32_t headerCompression = 0;
    std::vector<uint8_t> fileData;
    std::vector<uint8_t> decompressedTables;
    std::vector<char> labels;
    RcoNode mainTree;
    std::vector<const RcoNode *> models;
    std::vector<const RcoNode *> images;
    WaveScene scene;
    std::string extractionMethod = "tree";
};

using RcoDocResult = RcoResult<RcoDocument>;

RcoDocResult readRcoDocument(const std::vector<uint8_t> &data);
RcoResult<std::vector<uint8_t>> readResource(const RcoDocument &doc, const ResourceDesc &desc);
RcoResult<std::vector<uint8_t>> readNodeResource(const RcoDocument &doc, const RcoNode &node);

const RcoNode *findModelByLabel(const RcoDocument &doc, const std::string &label);
WaveScene buildWaveScene(RcoDocument &doc);

} // namespace xmb::rco

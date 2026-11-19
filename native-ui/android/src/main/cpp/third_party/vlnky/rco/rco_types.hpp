// SPDX-FileCopyrightText: 2026 Pavel Švec
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace xmb::rco {

constexpr uint32_t kRcoSignature = 0x46525000u;
constexpr uint32_t kRcoNullPtr = 0xFFFFFFFFu;

constexpr int kTableMain = 1;
constexpr int kTableVsmx = 2;
constexpr int kTableText = 3;
constexpr int kTableImg = 4;
constexpr int kTableModel = 5;
constexpr int kTableSound = 6;
constexpr int kTableFont = 7;
constexpr int kTableObj = 8;
constexpr int kTableAnim = 9;

constexpr int kObjTypeModelObject = 14;

constexpr int kDataCompressionNone = 0;
constexpr int kDataCompressionZlib = 1;

constexpr int kRefModel = 0x403;
constexpr int kRefNone = 0xFFFF;

constexpr int kModelObjectExtraWords = 18;

#pragma pack(push, 1)
struct PrfHeader {
    uint32_t signature;
    uint32_t version;
    uint32_t nullField;
    uint32_t compression;
    uint32_t pMainTable;
    uint32_t pVsmxTable;
    uint32_t pTextTable;
    uint32_t pSoundTable;
    uint32_t pModelTable;
    uint32_t pImgTable;
    uint32_t pUnknown;
    uint32_t pFontTable;
    uint32_t pObjTable;
    uint32_t pAnimTable;
    uint32_t pTextData;
    uint32_t lTextData;
    uint32_t pLabelData;
    uint32_t lLabelData;
    uint32_t pEventData;
    uint32_t lEventData;
    uint32_t pTextPtrs;
    uint32_t lTextPtrs;
    uint32_t pImgPtrs;
    uint32_t lImgPtrs;
    uint32_t pModelPtrs;
    uint32_t lModelPtrs;
    uint32_t pSoundPtrs;
    uint32_t lSoundPtrs;
    uint32_t pObjPtrs;
    uint32_t lObjPtrs;
    uint32_t pAnimPtrs;
    uint32_t lAnimPtrs;
    uint32_t pImgData;
    uint32_t lImgData;
    uint32_t pSoundData;
    uint32_t lSoundData;
    uint32_t pModelData;
    uint32_t lModelData;
    uint32_t unknown[3];
};

struct RcoEntryHeader {
    uint16_t typeId;
    uint16_t blank;
    uint32_t labelOffset;
    uint32_t eHeadSize;
    uint32_t entrySize;
    uint32_t numSubentries;
    uint32_t nextEntryOffset;
    uint32_t prevEntryOffset;
    uint32_t parentTblOffset;
    uint32_t blanks[2];
};

struct ImgModelEntry {
    uint16_t format;
    uint16_t compression;
    uint32_t sizePacked;
    uint32_t offset;
    uint32_t sizeUnpacked;
};

struct HeaderComprInfo {
    uint32_t lenPacked;
    uint32_t lenUnpacked;
    uint32_t lenLongestText;
};

struct RcoReference {
    uint32_t type;
    uint32_t ptr;
};
#pragma pack(pop)

static_assert(sizeof(PrfHeader) == 0xA4, "PRF header size");

} // namespace xmb::rco

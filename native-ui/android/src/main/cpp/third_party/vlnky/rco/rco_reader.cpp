// SPDX-FileCopyrightText: 2026 Pavel Švec
// SPDX-License-Identifier: GPL-2.0-or-later

#include "rco_reader.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace xmb::rco {
namespace {

constexpr size_t kPrfHeaderSize = sizeof(PrfHeader);
constexpr size_t kEntryHeaderSize = sizeof(RcoEntryHeader);

bool canRead(const std::vector<uint8_t> &d, size_t off, size_t n)
{
    return off <= d.size() && n <= d.size() - off;
}

bool readU32(const std::vector<uint8_t> &d, size_t off, uint32_t &out)
{
    if (!canRead(d, off, 4))
        return false;
    std::memcpy(&out, d.data() + off, 4);
    return true;
}

uint32_t align4(uint32_t v)
{
    return (v + 3u) & ~3u;
}

std::string labelAt(const std::vector<char> &labels, uint32_t offset)
{
    if (offset == kRcoNullPtr || offset >= labels.size())
        return {};
    const char *p = labels.data() + offset;
    return std::string(p);
}

class IoView {
public:
    IoView(const std::vector<uint8_t> &file, const std::vector<uint8_t> *tables, uint32_t tablesBase)
        : m_file(file)
        , m_tables(tables)
        , m_tablesBase(tablesBase)
    {}

    bool seek(uint32_t pos)
    {
        m_pos = pos;
        return true;
    }

    uint32_t tell() const { return m_pos; }

    bool read(void *buf, size_t len)
    {
        if (m_tables && m_pos >= m_tablesBase) {
            const size_t rel = m_pos - m_tablesBase;
            if (rel + len > m_tables->size())
                return false;
            std::memcpy(buf, m_tables->data() + rel, len);
            m_pos += static_cast<uint32_t>(len);
            return true;
        }
        if (m_pos + len > m_file.size())
            return false;
        std::memcpy(buf, m_file.data() + m_pos, len);
        m_pos += static_cast<uint32_t>(len);
        return true;
    }

private:
    const std::vector<uint8_t> &m_file;
    const std::vector<uint8_t> *m_tables;
    uint32_t m_tablesBase;
    uint32_t m_pos = 0;
};

bool readImgModelExtra(IoView &io, const RcoEntryHeader &eh, RcoNode &out, uint32_t dataPoolBase)
{
    ImgModelEntry ime{};
    if (!io.read(&ime, sizeof(uint32_t)))
        return false;

    const uint16_t compression = ime.compression & 0xFF;
    size_t extraSize = sizeof(ImgModelEntry);
    if (compression == kDataCompressionNone) {
        if (!eh.nextEntryOffset || eh.nextEntryOffset < kEntryHeaderSize + extraSize)
            extraSize -= sizeof(uint32_t);
    }

    const size_t tail = extraSize - sizeof(uint32_t);
    if (tail > 0 && !io.read(reinterpret_cast<uint8_t *>(&ime) + sizeof(uint32_t), tail))
        return false;

    out.resource.valid = true;
    out.resource.format = ime.format;
    out.resource.compression = compression;
    out.resource.packedSize = ime.sizePacked;
    out.resource.unpackedSize = ime.sizeUnpacked;
    out.resource.fileOffset = ime.offset + dataPoolBase;
    return true;
}

WaveSceneTransform parseModelObjectExtra(const std::vector<uint8_t> &extra)
{
    WaveSceneTransform t;
    if (extra.size() < 13 * 4)
        return t;
    auto f = [&](int idx) -> float {
        float v = 0;
        std::memcpy(&v, extra.data() + idx * 4, 4);
        return v;
    };
    t.posX = f(0);
    t.posY = f(1);
    t.posZ = f(2);
    t.redScale = f(3);
    t.greenScale = f(4);
    t.blueScale = f(5);
    t.alphaScale = f(6);
    t.width = f(7);
    t.height = f(8);
    t.depth = f(9);
    t.scaleWidth = f(10);
    t.scaleHeight = f(11);
    t.scaleDepth = f(12);
    return t;
}

constexpr int kMaxTreeDepth = 16;
constexpr uint32_t kMaxObjExtraBytes = 0x400;

// Fallback extra sizes (bytes) for leaf objects that are the last sibling, where the
// layout itself does not reveal the size. Values from PSP firmware RCO v0x71 files.
uint32_t fallbackObjExtraBytes(int objType)
{
    switch (objType) {
    case 1: return 9 * 4;                         // Page
    case 2: return 19 * 4;                        // Plane
    case kObjTypeModelObject: return kModelObjectExtraWords * 4;
    default: return 0;
    }
}

bool readEntry(IoView &io, const std::vector<char> &labels, const PrfHeader &header, RcoNode &out, int depth)
{
    if (depth > kMaxTreeDepth)
        return false;

    out.fileOffset = io.tell();
    RcoEntryHeader eh{};
    if (!io.read(&eh, kEntryHeaderSize))
        return false;

    out.tableId = (eh.typeId >> 8) & 0xFF;
    out.type = eh.typeId & 0xFF;
    out.label = labelAt(labels, eh.labelOffset);

    // Offset of the first child relative to this entry. The file stores it in entrySize
    // (header + extra) for entries that have subentries; main tables use 0x28.
    uint32_t firstChildRel = kEntryHeaderSize;
    if (eh.numSubentries > 0 && eh.entrySize >= kEntryHeaderSize)
        firstChildRel = eh.entrySize;

    if (out.tableId == kTableImg || out.tableId == kTableModel) {
        if (out.type > 0) {
            const uint32_t pool = out.tableId == kTableImg ? header.pImgData : header.pModelData;
            if (!readImgModelExtra(io, eh, out, pool))
                return false;
        }
    } else if (out.tableId == kTableObj && out.type > 0) {
        uint32_t extraBytes = 0;
        if (eh.numSubentries > 0)
            extraBytes = firstChildRel - kEntryHeaderSize;
        else if (eh.nextEntryOffset >= kEntryHeaderSize)
            extraBytes = eh.nextEntryOffset - kEntryHeaderSize;
        else
            extraBytes = fallbackObjExtraBytes(out.type);
        extraBytes = std::min(extraBytes, kMaxObjExtraBytes);
        if (extraBytes > 0) {
            out.extra.resize(extraBytes);
            if (!io.read(out.extra.data(), out.extra.size()))
                out.extra.clear();
        }
    }

    uint32_t childPos = out.fileOffset + firstChildRel;
    for (uint32_t i = 0; i < eh.numSubentries; ++i) {
        io.seek(childPos);
        RcoNode child;
        if (!readEntry(io, labels, header, child, depth + 1))
            break;
        const uint32_t next = child.nextRel;
        out.children.push_back(std::move(child));
        if (next == 0 || next < kEntryHeaderSize)
            break;
        childPos += next;
    }

    out.nextRel = eh.nextEntryOffset;
    return true;
}

void collectNodes(const RcoNode &node, std::vector<const RcoNode *> &models, std::vector<const RcoNode *> &images,
                  std::vector<const RcoNode *> &modelObjects)
{
    if (node.tableId == kTableModel && node.type == 1 && node.resource.valid)
        models.push_back(&node);
    if (node.tableId == kTableImg && node.type == 1 && node.resource.valid)
        images.push_back(&node);

    if (node.tableId == kTableObj && node.type == kObjTypeModelObject)
        modelObjects.push_back(&node);

    for (const auto &c : node.children)
        collectNodes(c, models, images, modelObjects);
}

const RcoNode *findInTree(const RcoNode &node, const std::function<bool(const RcoNode &)> &pred)
{
    if (pred(node))
        return &node;
    for (const auto &c : node.children) {
        if (const RcoNode *f = findInTree(c, pred))
            return f;
    }
    return nullptr;
}

} // namespace

RcoDocResult readRcoDocument(const std::vector<uint8_t> &data)
{
    RcoDocResult result;
    if (data.size() < kPrfHeaderSize) {
        result.error = RcoError::BufferOverrun;
        return result;
    }

    auto &doc = result.value;
    doc.fileData = data;
    std::memcpy(&doc.header, data.data(), kPrfHeaderSize);

    if (doc.header.signature != kRcoSignature) {
        result.error = RcoError::InvalidPrfMagic;
        return result;
    }

    doc.headerCompression = doc.header.compression >> 4;

    const std::vector<uint8_t> *tablesPtr = nullptr;
    uint32_t tablesBase = 0;

    if (doc.headerCompression == kDataCompressionZlib) {
        if (data.size() < kPrfHeaderSize + sizeof(HeaderComprInfo)) {
            result.error = RcoError::BufferOverrun;
            return result;
        }
        HeaderComprInfo ci{};
        std::memcpy(&ci, data.data() + kPrfHeaderSize, sizeof(ci));
        const size_t packedOff = kPrfHeaderSize + sizeof(HeaderComprInfo);
        if (packedOff + ci.lenPacked > data.size()) {
            result.error = RcoError::BufferOverrun;
            return result;
        }
        doc.decompressedTables.resize(ci.lenUnpacked);
        uLongf outLen = ci.lenUnpacked;
        const int zr = uncompress(doc.decompressedTables.data(), &outLen, data.data() + packedOff,
                                  static_cast<uLong>(ci.lenPacked));
        if (zr != Z_OK && zr != Z_BUF_ERROR) {
            result.error = RcoError::BufferOverrun;
            return result;
        }
        tablesPtr = &doc.decompressedTables;
        tablesBase = kPrfHeaderSize;
    }

    if (doc.header.lLabelData && doc.header.pLabelData < data.size()) {
        const size_t len = std::min<size_t>(doc.header.lLabelData, data.size() - doc.header.pLabelData);
        doc.labels.assign(reinterpret_cast<const char *>(data.data() + doc.header.pLabelData),
                          reinterpret_cast<const char *>(data.data() + doc.header.pLabelData + len));
    }

    IoView io(data, tablesPtr, tablesBase);
    io.seek(doc.header.pMainTable);
    if (!readEntry(io, doc.labels, doc.header, doc.mainTree, 0)) {
        result.error = RcoError::BufferOverrun;
        return result;
    }

    doc.scene = buildWaveScene(doc);
    return result;
}

WaveScene buildWaveScene(RcoDocument &doc)
{
    WaveScene scene;
    doc.models.clear();
    doc.images.clear();
    std::vector<const RcoNode *> modelObjects;
    collectNodes(doc.mainTree, doc.models, doc.images, modelObjects);

    const RcoNode *mo = nullptr;
    for (const RcoNode *n : modelObjects) {
        if (n->label == "default_theme_model") {
            mo = n;
            break;
        }
    }
    if (!mo && !modelObjects.empty())
        mo = modelObjects.front();

    if (mo) {
        scene.foundModelObject = true;
        scene.modelObject = parseModelObjectExtra(mo->extra);
        scene.modelLabel = "mdl_bg";
        // Locate the model reference {type=0x403, ptr} among the object's reference words.
        for (size_t w = 13; (w + 2) * 4 <= mo->extra.size(); ++w) {
            RcoReference modelRef{};
            std::memcpy(&modelRef, mo->extra.data() + w * 4, sizeof(modelRef));
            if (modelRef.type == kRefModel && modelRef.ptr != kRcoNullPtr) {
                for (const RcoNode *m : doc.models) {
                    if (m->fileOffset == modelRef.ptr) {
                        scene.modelLabel = m->label;
                        scene.modelEntry = m;
                        break;
                    }
                }
            }
            if (scene.modelEntry)
                break;
        }
    }

    if (!scene.modelEntry) {
        for (const RcoNode *m : doc.models) {
            if (m->label == scene.modelLabel || m->label == "mdl_bg") {
                scene.modelEntry = m;
                scene.modelLabel = m->label;
                break;
            }
        }
    }
    if (!scene.modelEntry && !doc.models.empty()) {
        scene.modelEntry = doc.models.front();
        scene.modelLabel = doc.models.front()->label;
    }

    if (!doc.images.empty())
        scene.imageEntry = doc.images.front();

    return scene;
}

const RcoNode *findModelByLabel(const RcoDocument &doc, const std::string &label)
{
    for (const RcoNode *m : doc.models) {
        if (m->label == label)
            return m;
    }
    return nullptr;
}

RcoResult<std::vector<uint8_t>> readResource(const RcoDocument &doc, const ResourceDesc &desc)
{
    RcoResult<std::vector<uint8_t>> result;
    if (!desc.valid || desc.fileOffset >= doc.fileData.size()) {
        result.error = RcoError::BufferOverrun;
        return result;
    }

    const uint32_t packed = desc.packedSize ? desc.packedSize : desc.unpackedSize;
    if (desc.fileOffset + packed > doc.fileData.size()) {
        result.error = RcoError::BufferOverrun;
        return result;
    }

    std::vector<uint8_t> packedData(doc.fileData.begin() + desc.fileOffset,
                                    doc.fileData.begin() + desc.fileOffset + packed);

    if (desc.compression == kDataCompressionZlib) {
        uLongf outLen = desc.unpackedSize ? desc.unpackedSize : packed * 4;
        result.value.resize(outLen);
        const int zr = uncompress(result.value.data(), &outLen, packedData.data(), static_cast<uLong>(packedData.size()));
        if (zr != Z_OK && zr != Z_BUF_ERROR) {
            result.error = RcoError::BufferOverrun;
            result.value.clear();
            return result;
        }
        result.value.resize(outLen);
    } else {
        result.value = std::move(packedData);
    }
    return result;
}

RcoResult<std::vector<uint8_t>> readNodeResource(const RcoDocument &doc, const RcoNode &node)
{
    return readResource(doc, node.resource);
}

} // namespace xmb::rco

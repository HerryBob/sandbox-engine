#include "shared/chunk/ChunkData.hpp"

#include <algorithm>
#include <cstring>

#include "shared/chunk/ChunkFormat.hpp"

// ============================================================================
// ChunkData 存档序列化（格式定义见 docs\DOCS自制版\存档系统设计.md）
// ============================================================================
//
// 为什么放在这里而不是单独的 SaveFormat 模块：
//   二进制布局与**内存布局**一一对应（4/8/16 位索引逐字节搬），格式知识必须与
//   存储实现放在一起才不会两边漂移。存档模块（step ②）只负责目录/文件名/IO。
namespace engine::shared {

namespace {

using chunkfmt::kFlagAllAir;
using chunkfmt::kFormatVersion;
using chunkfmt::kHeaderSize;
using chunkfmt::kMagic;
using chunkfmt::kMaxSectionCount;
using chunkfmt::kTagIndices;
using chunkfmt::kTagPalette;

// ---- 小端读写（不依赖平台字节序）----
inline void putU16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}
inline void putU32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
}
inline void writeU16At(std::uint8_t* p, std::uint16_t x) {
    p[0] = static_cast<std::uint8_t>(x & 0xFF);
    p[1] = static_cast<std::uint8_t>((x >> 8) & 0xFF);
}
inline void writeU32At(std::uint8_t* p, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF);
}
inline std::uint16_t readU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}
inline std::uint32_t readU32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    return v;
}

// FNV-1a 32：只用于发现截断/写中断（不是密码学校验）
inline std::uint32_t fnv1a32(const std::uint8_t* p, std::size_t n) {
    std::uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

} // namespace

void ChunkData::serialize(std::vector<std::uint8_t>& out,
                          int chunkX, int chunkY, int chunkZ, std::uint32_t genVer) const {
    out.clear();

    const bool allAir = isAllAir();
    const std::uint16_t flags = allAir ? kFlagAllAir : 0;
    const std::uint16_t sectionCount = allAir ? 0 : 2;   // Palette + Indices

    // ---- 头（32 字节，先按位置写，段表和正文之后再填 checksum）----
    const std::size_t headerAt = out.size();
    putU32(out, kMagic);
    putU16(out, kFormatVersion);
    putU16(out, kHeaderSize);
    putU32(out, genVer);
    putU32(out, static_cast<std::uint32_t>(chunkX));
    putU32(out, static_cast<std::uint32_t>(chunkY));
    putU32(out, static_cast<std::uint32_t>(chunkZ));
    putU16(out, sectionCount);
    putU16(out, flags);
    putU32(out, 0);   // checksum 占位

    if (allAir) {
        // 全空气：零段、正文为空，但 checksum 仍必须写（空正文的 FNV 是偏移基数）。
        // 漏写会让读入端认为"校验失败"→ 所有空 chunk 都被当成损坏（第一次实现就踩了）。
        const std::uint32_t emptyChecksum = fnv1a32(out.data() + out.size(), 0);
        writeU32At(out.data() + headerAt + 28, emptyChecksum);
        return;
    }

    // ---- 段表（sectionCount × 8 字节：tag u16 / len u32 / pad u16）----
    const std::size_t tableAt = out.size();
    for (int i = 0; i < sectionCount; ++i) {
        putU16(out, 0);
        putU32(out, 0);
        putU16(out, 0);
    }
    const std::size_t bodyAt = out.size();

    // ---- 规范化：调色板按类型 id 排序 + 重映射索引 ----
    // 为什么必须做：内存里的调色板顺序取决于"方块被写入的先后"，而存档应该只取决于
    // **世界内容**。不规范化的话，同一个世界存两次字节可能不同（实测：加载后重存有
    // 2 个 chunk 的字节变了），既没法做存档 diff，也没法用"字节相等"当回归判据。
    // 代价：写盘时一次 4096 遍历 + 8KB 临时表（只在写盘时发生，可忽略）。
    std::vector<PaletteEntry> canonPalette;
    canonPalette.reserve(palette_.size());
    for (const PaletteEntry e : palette_) canonPalette.push_back(e);
    std::sort(canonPalette.begin(), canonPalette.end());
    canonPalette.erase(std::unique(canonPalette.begin(), canonPalette.end()), canonPalette.end());
    if (canonPalette.empty()) canonPalette.push_back(0);   // 兜底：至少要有 Air

    // 旧索引 → 新索引
    std::vector<std::uint16_t> remap(palette_.size(), 0);
    for (std::size_t oldIdx = 0; oldIdx < palette_.size(); ++oldIdx) {
        const auto it = std::lower_bound(canonPalette.begin(), canonPalette.end(), palette_[oldIdx]);
        remap[oldIdx] = static_cast<std::uint16_t>(it - canonPalette.begin());
    }

    // 位宽按**规范化后**的调色板大小决定（可能比内存里的更小，例如内存留了废弃条目）
    std::uint8_t writeBits = 4;
    if (canonPalette.size() > 256) writeBits = 16;
    else if (canonPalette.size() > 16) writeBits = 8;

    // ---- 段 1：Palette ----
    const std::size_t paletteLenAt = out.size();
    putU16(out, static_cast<std::uint16_t>(canonPalette.size()));
    for (const PaletteEntry e : canonPalette) putU16(out, e);
    const std::uint32_t paletteLen = static_cast<std::uint32_t>(out.size() - paletteLenAt);

    // ---- 段 2：Indices（与内存布局逐字节一致，不做重打包）----
    // 段正文 = bits u16 + pad u16 + dataLen u32 + data（段表里的 tag/len 是另一处，
    // 这里**不能**再写一份段头 —— 第一版多写了 8 字节占位，读回时 bits 读到 0）。
    const std::size_t idxLenAt = out.size();
    putU16(out, static_cast<std::uint16_t>(writeBits));
    putU16(out, 0);                                       // pad
    // 按 writeBits 逐格重打包（不再"逐字节搬内存"：因为规范化可能改变位宽）
    if (writeBits == 16) {
        putU32(out, static_cast<std::uint32_t>(CHUNK_VOLUME * 2));
        for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i)
            putU16(out, remap[paletteIndexAt(i)]);
    } else {
        std::vector<std::uint8_t> packed;
        if (writeBits == 4) {
            packed.assign(static_cast<std::size_t>(CHUNK_VOLUME) / 2, 0);
            for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i) {
                const std::uint16_t v = remap[paletteIndexAt(i)] & 0x0F;
                std::uint8_t& b = packed[static_cast<std::size_t>(i >> 1)];
                if (i & 1) b = static_cast<std::uint8_t>((b & 0x0F) | (v << 4));
                else       b = static_cast<std::uint8_t>((b & 0xF0) | v);
            }
        } else {
            packed.resize(static_cast<std::size_t>(CHUNK_VOLUME));
            for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i)
                packed[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(remap[paletteIndexAt(i)]);
        }
        putU32(out, static_cast<std::uint32_t>(packed.size()));
        out.insert(out.end(), packed.begin(), packed.end());
    }
    const std::uint32_t idxLen = static_cast<std::uint32_t>(out.size() - idxLenAt);

    // ---- 回填段表 ----
    std::uint8_t* table = out.data() + tableAt;
    writeU16At(table + 0, kTagPalette);
    writeU32At(table + 2, paletteLen);
    writeU16At(table + 6, 0);
    writeU16At(table + 8, kTagIndices);
    writeU32At(table + 10, idxLen);
    writeU16At(table + 14, 0);

    // ---- 回填 checksum（覆盖段表 + 正文）----
    const std::uint32_t checksum = fnv1a32(out.data() + tableAt, out.size() - tableAt);
    writeU32At(out.data() + headerAt + 28, checksum);
}

bool ChunkData::deserialize(const std::uint8_t* data, std::size_t size,
                            int chunkX, int chunkY, int chunkZ,
                            ChunkData& out, std::uint32_t* genVerOut) {
    if (data == nullptr || size < kHeaderSize) return false;

    // ---- ① 头 ----
    if (readU32(data) != kMagic) return false;
    if (readU16(data + 4) != kFormatVersion) return false;     // 格式版本不符 → 拒读
    if (readU16(data + 6) != kHeaderSize) return false;
    const std::uint32_t genVer  = readU32(data + 8);
    const int cx = static_cast<int>(readU32(data + 12));
    const int cy = static_cast<int>(readU32(data + 16));
    const int cz = static_cast<int>(readU32(data + 20));
    const std::uint16_t sectionCount = readU16(data + 24);
    const std::uint16_t flags        = readU16(data + 26);
    const std::uint32_t checksum     = readU32(data + 28);

    // ---- ② 坐标必须与文件名一致（防止文件放错目录）----
    if (cx != chunkX || cy != chunkY || cz != chunkZ) return false;

    // ---- ③ 段表边界 ----
    if (sectionCount > kMaxSectionCount) return false;
    const std::size_t tableBytes = static_cast<std::size_t>(sectionCount) * 8;
    if (kHeaderSize + tableBytes > size) return false;

    // ---- ④ checksum ----
    if (fnv1a32(data + kHeaderSize, size - kHeaderSize) != checksum) return false;

    if ((flags & kFlagAllAir) != 0) {
        out = ChunkData{};   // 全空气：默认构造即空
        if (genVerOut) *genVerOut = genVer;
        return true;
    }

    // ---- ⑤ 解析段（未知 tag 按 len 跳过）----
    ChunkData result;                     // 先在临时对象上改，成功才提交
    std::uint32_t foundBits = 0;
    bool havePalette = false;
    bool haveIndices = false;

    std::size_t cursor = kHeaderSize + tableBytes;
    for (std::uint16_t s = 0; s < sectionCount; ++s) {
        const std::uint8_t* entry = data + kHeaderSize + static_cast<std::size_t>(s) * 8;
        const std::uint16_t tag = readU16(entry);
        const std::uint32_t len = readU32(entry + 2);
        if (len > size - cursor) return false;   // 越界
        const std::uint8_t* body = data + cursor;

        if (tag == kTagPalette) {
            if (len < 2) return false;
            const std::uint16_t count = readU16(body);
            if (count == 0 || len < 2u + static_cast<std::uint32_t>(count) * 2u) return false;
            result.palette_.clear();
            result.palette_.reserve(count);
            for (std::uint16_t i = 0; i < count; ++i)
                result.palette_.push_back(readU16(body + 2 + static_cast<std::size_t>(i) * 2));
            havePalette = true;
        } else if (tag == kTagIndices) {
            if (len < 8) return false;
            const std::uint16_t bits = readU16(body);
            const std::uint32_t dataLen = readU32(body + 4);
            if (bits != 4 && bits != 8 && bits != 16) return false;
            const std::size_t expect = static_cast<std::size_t>(CHUNK_VOLUME) * bits / 8;
            if (dataLen != expect || len < 8u + dataLen) return false;
            result.bits_ = static_cast<std::uint8_t>(bits);
            if (bits == 16) {
                result.wide_.resize(static_cast<std::size_t>(CHUNK_VOLUME));
                for (std::size_t i = 0; i < result.wide_.size(); ++i)
                    result.wide_[i] = readU16(body + 8 + i * 2);
                result.packed_.clear();
            } else {
                result.packed_.assign(body + 8, body + 8 + dataLen);
                result.wide_.clear();
            }
            haveIndices = true;
            foundBits = bits;
        }
        // 未知 tag：跳过（这是段表存在的意义 —— 加新段不必升格式版本）
        cursor += len;
    }

    if (!havePalette || !haveIndices) return false;

    // ---- ⑥ 一致性：索引值必须落在调色板范围内 ----
    const std::size_t paletteSize = result.palette_.size();
    for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i) {
        if (result.paletteIndexAt(i) >= paletteSize) return false;
    }
    (void)foundBits;

    out = std::move(result);
    if (genVerOut) *genVerOut = genVer;
    return true;
}

} // namespace engine::shared

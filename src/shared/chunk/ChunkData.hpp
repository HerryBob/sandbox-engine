#pragma once
#include "shared/block/Block.hpp"
#include "shared/chunk/ChunkPos.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::shared {

// ---- 区块化存储（数据层 ②，服务端主世界存储单元）----
// 主世界静态方块按 16³ 立方体 chunk 组织（Minetest MapBlock 同款尺寸）。
// 定位 O(1)：坐标 >>4 找 chunk，&15 找块内；空 chunk 不占内存（稀疏映射）。
// 每个 chunk 用调色板存：局部字典记录本块出现过的方块 id，格子里只存字典索引，
// 索引宽度按字典大小在 4 / 8 / 16 位之间自适应（典型地表 chunk 只有 2~4 项）。
// 主世界方块 contraptionId 恒 -1，无需冗余存储；刚体内方块不进 chunk
// （复合体 = 动态渲染单元，业务侧独立容器管理，见 BusinessWorld）。
// 修改跟踪：ChunkData 的 dirty 位掩码记录"因何变更"（Minetest raiseModified 思想），
// 是变更事件流（数据层 ② 第四项）的服务端种子。
// 用法：放置 markDirty(DIRTY_PLACED) / 破坏 markDirty(DIRTY_REMOVED) / 生成装载 markDirty(DIRTY_GENERATED)，
//       存档写盘成功后 clearDirty()。位值定义在 BlockActions.hpp。

// 一个 chunk 的方块数据：调色板（局部字典）+ 紧凑索引。
// 读取 = 一次字典查表；写入时条目不在字典里就追加，超出当前位宽容量则扩宽一次。
class ChunkData {
public:
    ChunkData();

    // 块内读写：坐标是块内坐标（0..15），越界由 &15 落到块内，与 chunkIndexOf 一致。
    PaletteEntry paletteGet(int x, int y, int z) const;
    PaletteEntry paletteGet(const BlockPos& p) const { return paletteGet(p.x, p.y, p.z); }
	// 块内写入：会自动扩宽索引位宽（4→8→16），并在 palette_ 里追加新条目。
    void paletteSet(int x, int y, int z, PaletteEntry e);
    void paletteSet(const BlockPos& p, PaletteEntry e) { paletteSet(p.x, p.y, p.z, e); }

    // 按数组索引直取（快照遍历用，省一次坐标换算）。
    PaletteEntry paletteGetByIndex(std::int32_t i) const { return palette_[paletteIndexAt(i)]; }

    // 本 chunk 是否全空气（生成装载时判空用；全空气 chunk 不入表）。
    bool chunkIsAllAir() const;
    // 非空气方块数。
    std::size_t chunkNonAirCount() const;
    // 索引数组 + 字典的实际占用字节（内存账用）。
    std::size_t chunkByteSize() const;
    // 压缩：丢掉字典里没有任何格子引用的条目，重映射索引，再把位宽降到最小档。
    void chunkCompact();

    std::uint8_t chunkIndexBits() const { return bits_; }
    std::size_t paletteSize() const { return palette_.size(); }

    // 变更原因位掩码（0 = 干净）；位值见 BlockActions.hpp 的 DIRTY_*
    std::uint32_t dirty() const noexcept { return dirty_; }

    // 块内坐标 → 数组索引（x/y/z 低 4 位各占一段）。
    // 与 chunkLocalOf 严格互逆：改其中一处必须同步改另一处，否则快照遍历会坐标错位。
    static std::int32_t chunkIndexOf(int x, int y, int z) {
        return (z & 15) * 256 + (y & 15) * 16 + (x & 15);
    }
    // 数组索引 → 块内偏移（x/y/z ∈ 0..15）。快照遍历用的唯一解码点。
    static BlockPos chunkLocalOf(std::int32_t i) {
        return BlockPos{i & 15, (i >> 4) & 15, i >> 8};
    }

    // 置位变更原因；|= 累加，同一区块可同时带多个原因。只写不读——消费者是区块存档。
    void markDirty(std::uint32_t reason) noexcept { dirty_ |= reason; }
    // 清空全部变更原因（0=干净）。与 World::dirtyChunks_（渲染重建队列）无关；
    // 当前无调用点，等存档写盘成功后才会被调。
    void clearDirty() noexcept { dirty_ = 0; }

private:
    // 读第 i 格的字典下标；i 恒为 0..4095（chunkIndexOf 的 &15 保证），此处不做越界检查。
    std::uint16_t paletteIndexAt(std::int32_t i) const;
    // 写第 i 格为字典下标 v；v 必须小于 1 << bits_，超出由 &0x0F 静默截断（由 paletteFindOrAdd 保证不超出）。
    void paletteIndexSet(std::int32_t i, std::uint16_t v);
    // 条目 → 字典下标；不存在则追加，超过当前位宽容量则扩宽。
    std::uint16_t paletteFindOrAdd(PaletteEntry e);
    // 4→8 / 8→16：按新位宽重建整张索引数组。
    void chunkChangeIndexBits(std::uint8_t newBits);

    std::vector<PaletteEntry> palette_;   // 局部字典，初值只有 Air
    std::vector<std::uint8_t> packed_;    // 4 位：每字节 2 格；8 位：每字节 1 格
    std::vector<std::uint16_t> wide_;     // 16 位：每格 1 项
    std::uint8_t bits_ = 4;               // 当前索引位宽：4 / 8 / 16
    std::uint32_t dirty_ = 0;             // 变更原因位掩码，只能通过 markDirty/clearDirty 修改
};

inline ChunkData::ChunkData()
    : palette_(1, kAirEntry),
      packed_(static_cast<std::size_t>(CHUNK_VOLUME) / 2, 0),
      bits_(4) {}

inline std::uint16_t ChunkData::paletteIndexAt(std::int32_t i) const {
    if (bits_ == 4) {
        const std::uint8_t b = packed_[static_cast<std::size_t>(i >> 1)];
        return static_cast<std::uint16_t>((i & 1) ? (b >> 4) : (b & 0x0F));
    }
    if (bits_ == 8) return static_cast<std::uint16_t>(packed_[static_cast<std::size_t>(i)]);

    return wide_[static_cast<std::size_t>(i)];
}

inline void ChunkData::paletteIndexSet(std::int32_t i, std::uint16_t v) {
    if (bits_ == 4) {
        std::uint8_t& b = packed_[static_cast<std::size_t>(i >> 1)];
        if (i & 1) b = static_cast<std::uint8_t>((b & 0x0F) | ((v & 0x0F) << 4));
        else       b = static_cast<std::uint8_t>((b & 0xF0) | (v & 0x0F));
    } else if (bits_ == 8) {
        packed_[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v);
    } else {
        wide_[static_cast<std::size_t>(i)] = v;
    }
}

inline std::uint16_t ChunkData::paletteFindOrAdd(PaletteEntry e) {
    const std::size_t n = palette_.size();
    for (std::size_t k = 0; k < n; ++k)
        if (palette_[k] == e) return static_cast<std::uint16_t>(k);
    palette_.push_back(e);
    // 字典项数超过当前位宽能编码的上限 → 升一档
    if (palette_.size() > (std::size_t{1} << bits_))
        chunkChangeIndexBits(bits_ == 4 ? 8 : 16);
    return static_cast<std::uint16_t>(palette_.size() - 1);
}

inline PaletteEntry ChunkData::paletteGet(int x, int y, int z) const {
    return paletteGetByIndex(chunkIndexOf(x, y, z));
}

inline void ChunkData::paletteSet(int x, int y, int z, PaletteEntry e) {
    paletteIndexSet(chunkIndexOf(x, y, z), paletteFindOrAdd(e));
}

inline bool ChunkData::chunkIsAllAir() const {
    if (palette_.size() == 1) return palette_[0] == kAirEntry;
    // 字典里混有非空气条目（生成后又删过方块等）：逐格确认确实都指向 Air
    for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i)
        if (palette_[paletteIndexAt(i)] != kAirEntry) return false;
    return true;
}

inline std::size_t ChunkData::chunkNonAirCount() const {
    std::size_t n = 0;
    for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i)
        if (palette_[paletteIndexAt(i)] != kAirEntry) ++n;
    return n;
}

inline std::size_t ChunkData::chunkByteSize() const {
    const std::size_t idxBytes =
        (bits_ == 16) ? wide_.size() * sizeof(std::uint16_t) : packed_.size();
    return idxBytes + palette_.size() * sizeof(PaletteEntry);
}

inline void ChunkData::chunkChangeIndexBits(std::uint8_t newBits) {
    if (newBits == bits_) return;
    // 字典项数必须能塞进新位宽，否则降档会让下标被截断，直接放弃
    if (palette_.size() > (std::size_t{ 1 } << newBits)) return;
    // 先按旧位宽读出全部索引，再换容器写回（索引值不变，只换编码方式）
    std::vector<std::uint16_t> tmp(static_cast<std::size_t>(CHUNK_VOLUME), 0);
    for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i)
        tmp[static_cast<std::size_t>(i)] = paletteIndexAt(i);

    // 三档各自先构造新容器，构造成功后才改 bits_，分配失败时对象保持原状
    if (newBits == 4) {
        std::vector<std::uint8_t> next(static_cast<std::size_t>(CHUNK_VOLUME) / 2, 0);
        for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i) {
            std::uint8_t& b = next[static_cast<std::size_t>(i >> 1)];
            const std::uint8_t v = static_cast<std::uint8_t>(tmp[static_cast<std::size_t>(i)] & 0x0F);
            if (i & 1) b = static_cast<std::uint8_t>((b & 0x0F) | (v << 4));
            else       b = static_cast<std::uint8_t>((b & 0xF0) | v);
        }
        bits_ = newBits;
        packed_.swap(next);
        std::vector<std::uint16_t>().swap(wide_);
    }
    else if (newBits == 8) {
        std::vector<std::uint8_t> next(static_cast<std::size_t>(CHUNK_VOLUME), 0);
        for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i)
            next[static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(tmp[static_cast<std::size_t>(i)]);
        bits_ = newBits;
        packed_.swap(next);
        std::vector<std::uint16_t>().swap(wide_);
    } else {
        std::vector<std::uint16_t> next(static_cast<std::size_t>(CHUNK_VOLUME), 0);
        for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i)
            next[static_cast<std::size_t>(i)] = tmp[static_cast<std::size_t>(i)];
        bits_ = newBits;
        wide_.swap(next);
        std::vector<std::uint8_t>().swap(packed_);
    }
}

inline void ChunkData::chunkCompact() {
    const std::size_t n = palette_.size();
    if (n <= 1) return;   // 字典只有 Air，无可压缩项

    // ① 标记哪些字典项还被格子引用；Air（0 号）始终保留
    std::vector<std::uint8_t> used(n, 0);
    used[0] = 1;
    for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i) {
        const std::uint16_t k = paletteIndexAt(i);
        if (k < n) used[k] = 1;
    }

    // ② 重建字典，同时建立旧下标 → 新下标映射
    std::vector<std::uint16_t> remap(n, 0);
    std::vector<PaletteEntry> nextPal;
    nextPal.reserve(n);
    for (std::size_t k = 0; k < n; ++k) {
        if (used[k] == 0) continue;
        remap[k] = static_cast<std::uint16_t>(nextPal.size());
        nextPal.push_back(palette_[k]);
    }
    if (nextPal.size() == n) return;   // 一项都没丢，不用动

    // ③ 先按旧字典重写索引，再换字典，最后按新字典能容纳的最小位宽重建索引数组
    for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i) {
        const std::uint16_t k = paletteIndexAt(i);
        paletteIndexSet(i, (k < n) ? remap[k] : 0);
    }
    palette_.swap(nextPal);

    const std::size_t cap = palette_.size();
    chunkChangeIndexBits((cap <= 16) ? 4 : (cap <= 256 ? 8 : 16));
}

} // namespace engine::shared

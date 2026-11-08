#pragma once

#include <cstddef>
#include <cstdint>
#include <functional> // std::hash 特化（BlockPos 作 unordered_map key 用）
#include <vector>

namespace engine::shared {

// 方块类型：只保留 Air 作为"空"哨兵（id 0）。
// 具体方块的 id 由方块注册表在运行期分配（内置方块由 mods/blocks.dll 注册），
// 引擎代码不得假设任何固定 id —— 要引用具体方块时用 registry::g_builtinBlockIds
// 或 registry::findBlockIdByName。
enum class BlockType : std::uint16_t {
    Air = 0,   // 空气（无方块）
};

// 调色板条目：方块 id；block state 落地后改存全局 state id，类型不变。
using PaletteEntry = std::uint32_t;

// 空气条目（比较用，省掉每处 static_cast）。
inline constexpr PaletteEntry kAirEntry = static_cast<PaletteEntry>(BlockType::Air);

// 方块坐标：整数格坐标（世界格子坐标系）。
struct BlockPos {
    int x = 0;
    int y = 0;
    int z = 0;

    // 手写逐成员比较（显式清晰；坐标映射 key 需要）。
    bool operator==(const BlockPos& rhs) const {
        return x == rhs.x && y == rhs.y && z == rhs.z;
    }
};

// 一个方块 = 类型 + 位置 + 所属域。
// contraptionId：-1 = 主世界静态方块（pos 为世界坐标）；
//                ≥0 = 刚体内方块（pos 为刚体局部坐标，飞船参考系）。
// 原型阶段 Block 是纯数据（无逻辑）；
// 后续 Contraption 打包/解包由"方块列表"构成（文档 7.5 第 2 步）。
struct Block {
    BlockType type = BlockType::Air;
    BlockPos  pos;
    int       contraptionId = -1; // -1=主世界，≥0=刚体（局部坐标）
};

// ---- 区块化存储（数据层 ②，服务端主世界存储单元）----
// 主世界静态方块按 16³ 立方体 chunk 组织（Minetest MapBlock 同款尺寸）。
// 定位 O(1)：坐标 >>4 找 chunk，&15 找块内；空 chunk 不占内存（稀疏映射）。
// 每个 chunk 用调色板存：局部字典记录本块出现过的方块 id，格子里只存字典索引，
// 索引宽度按字典大小在 4 / 8 / 16 位之间自适应（典型地表 chunk 只有 2~4 项）。
// 主世界方块 contraptionId 恒 -1，无需冗余存储；刚体内方块不进 chunk
// （复合体 = 动态渲染单元，业务侧独立容器管理，见 BusinessWorld）。
// 修改跟踪：ChunkData.dirty 位掩码记录"因何变更"（Minetest raiseModified 思想），
// 是变更事件流（数据层 ② 第四项）的服务端种子。
constexpr int CHUNK_SIZE  = 16;                                            // 16³ 立方体
constexpr int CHUNK_VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;          // 4096

// 区块坐标 = 世界坐标 >> 4（每个 chunk 覆盖 [pos*16, pos*16+15]）。
struct ChunkPos {
    int x = 0;
    int y = 0;
    int z = 0;

    // 手写逐成员比较（与 BlockPos 一致，显式清晰）。
    bool operator==(const ChunkPos& rhs) const {
        return x == rhs.x && y == rhs.y && z == rhs.z;
    }
};

// 一个 chunk 的方块数据：调色板（局部字典）+ 紧凑索引。
// 读取 = 一次字典查表；写入时条目不在字典里就追加，超出当前位宽容量则扩宽一次。
class ChunkData {
public:
    ChunkData();

    // 块内读写：坐标是块内坐标（0..15），越界由 &15 落到块内，与 indexOf 一致。
    PaletteEntry get(int x, int y, int z) const;
    PaletteEntry get(const BlockPos& p) const { return get(p.x, p.y, p.z); }
    void set(int x, int y, int z, PaletteEntry e);
    void set(const BlockPos& p, PaletteEntry e) { set(p.x, p.y, p.z, e); }

    // 按数组索引直取（快照遍历用，省一次坐标换算）。
    PaletteEntry getAt(std::int32_t i) const { return palette_[indexAt(i)]; }

    // 本 chunk 是否全空气（生成装载时判空用；全空气 chunk 不入表）。
    bool isAllAir() const;
    // 非空气方块数。
    std::size_t nonAirCount() const;
    // 索引数组 + 字典的实际占用字节（内存账用）。
    std::size_t byteSize() const;

    std::uint8_t bits() const { return bits_; }
    std::size_t paletteSize() const { return palette_.size(); }

    std::uint32_t dirty = 0;   // 变更原因位掩码（0=干净）

    // 块内坐标 → 数组索引（x/y/z 低 4 位各占一段）。
    // 与 localOf 严格互逆：改其中一处必须同步改另一处，否则快照遍历会坐标错位。
    static std::int32_t indexOf(int x, int y, int z) {
        return (z & 15) * 256 + (y & 15) * 16 + (x & 15);
    }
    // 数组索引 → 块内偏移（x/y/z ∈ 0..15）。快照遍历用的唯一解码点。
    static BlockPos localOf(std::int32_t i) {
        return BlockPos{i & 15, (i >> 4) & 15, i >> 8};
    }

    void markDirty(std::uint32_t reason) { dirty |= reason; }
    void clearDirty()                     { dirty = 0; }

private:
    // 格索引：按当前位宽从 packed_ / wide_ 取。
    std::uint16_t indexAt(std::int32_t i) const;
    void setIndexAt(std::int32_t i, std::uint16_t v);
    // 条目 → 字典下标；不存在则追加，超过当前位宽容量则扩宽。
    std::uint16_t findOrAdd(PaletteEntry e);
    // 4→8 / 8→16：按新位宽重建整张索引数组。
    void growTo(std::uint8_t newBits);

    std::vector<PaletteEntry> palette_;   // 局部字典，初值只有 Air
    std::vector<std::uint8_t> packed_;    // 4 位：每字节 2 格；8 位：每字节 1 格
    std::vector<std::uint16_t> wide_;     // 16 位：每格 1 项
    std::uint8_t bits_ = 4;               // 当前索引位宽：4 / 8 / 16
};

inline ChunkData::ChunkData()
    : palette_(1, kAirEntry),
      packed_(static_cast<std::size_t>(CHUNK_VOLUME) / 2, 0),
      bits_(4) {}

inline std::uint16_t ChunkData::indexAt(std::int32_t i) const {
    if (bits_ == 4) {
        const std::uint8_t b = packed_[static_cast<std::size_t>(i >> 1)];
        return static_cast<std::uint16_t>((i & 1) ? (b >> 4) : (b & 0x0F));
    }
    if (bits_ == 8) return static_cast<std::uint16_t>(packed_[static_cast<std::size_t>(i)]);
    return wide_[static_cast<std::size_t>(i)];
}

inline void ChunkData::setIndexAt(std::int32_t i, std::uint16_t v) {
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

inline std::uint16_t ChunkData::findOrAdd(PaletteEntry e) {
    const std::size_t n = palette_.size();
    for (std::size_t k = 0; k < n; ++k)
        if (palette_[k] == e) return static_cast<std::uint16_t>(k);
    palette_.push_back(e);
    // 字典项数超过当前位宽能编码的上限 → 升一档
    if (palette_.size() > (std::size_t{1} << bits_))
        growTo(bits_ == 4 ? 8 : 16);
    return static_cast<std::uint16_t>(palette_.size() - 1);
}

inline PaletteEntry ChunkData::get(int x, int y, int z) const {
    return palette_[indexAt(indexOf(x, y, z))];
}

inline void ChunkData::set(int x, int y, int z, PaletteEntry e) {
    setIndexAt(indexOf(x, y, z), findOrAdd(e));
}

inline bool ChunkData::isAllAir() const {
    if (palette_.size() == 1) return palette_[0] == kAirEntry;
    // 字典里混有非空气条目（生成后又删过方块等）：逐格确认确实都指向 Air
    for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i)
        if (palette_[indexAt(i)] != kAirEntry) return false;
    return true;
}

inline std::size_t ChunkData::nonAirCount() const {
    std::size_t n = 0;
    for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i)
        if (palette_[indexAt(i)] != kAirEntry) ++n;
    return n;
}

inline std::size_t ChunkData::byteSize() const {
    const std::size_t idxBytes =
        (bits_ == 16) ? wide_.size() * sizeof(std::uint16_t) : packed_.size();
    return idxBytes + palette_.size() * sizeof(PaletteEntry);
}

inline void ChunkData::growTo(std::uint8_t newBits) {
    if (newBits == bits_) return;
    // 先按旧位宽读出全部索引，再换容器写回（索引值不变，只换编码方式）
    std::vector<std::uint16_t> tmp(static_cast<std::size_t>(CHUNK_VOLUME), 0);
    for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i)
        tmp[static_cast<std::size_t>(i)] = indexAt(i);

    bits_ = newBits;
    if (newBits == 8) {
        std::vector<std::uint8_t> next(static_cast<std::size_t>(CHUNK_VOLUME), 0);
        for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i)
            next[static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(tmp[static_cast<std::size_t>(i)]);
        packed_.swap(next);
        std::vector<std::uint16_t>().swap(wide_);
    } else {
        std::vector<std::uint16_t> next(static_cast<std::size_t>(CHUNK_VOLUME), 0);
        for (std::int32_t i = 0; i < CHUNK_VOLUME; ++i)
            next[static_cast<std::size_t>(i)] = tmp[static_cast<std::size_t>(i)];
        wide_.swap(next);
        std::vector<std::uint8_t>().swap(packed_);
    }
}


// 世界坐标 → 区块坐标（算术右移向下取整：-1 属于 chunk -1，块内 15）。
inline ChunkPos chunkOf(const BlockPos& p) {
    return ChunkPos{p.x >> 4, p.y >> 4, p.z >> 4};
}

} // namespace engine::shared

// std::hash 特化：让 BlockPos / ChunkPos 可作 unordered_map 的 key（坐标映射）。
namespace std {
template <>
struct hash<engine::shared::BlockPos> {
    size_t operator()(const engine::shared::BlockPos& p) const noexcept {
        // 简单混合散列：xyz 各占位混合，原型够用。
        size_t h = static_cast<size_t>(p.x);
        h = h * 31 + static_cast<size_t>(p.y);
        h = h * 31 + static_cast<size_t>(p.z);
        return h;
    }
};
template <>
struct hash<engine::shared::ChunkPos> {
    size_t operator()(const engine::shared::ChunkPos& p) const noexcept {
        // 与 BlockPos 同款混合散列（区块坐标也是三维整数坐标）。
        size_t h = static_cast<size_t>(p.x);
        h = h * 31 + static_cast<size_t>(p.y);
        h = h * 31 + static_cast<size_t>(p.z);
        return h;
    }
};
} // namespace std

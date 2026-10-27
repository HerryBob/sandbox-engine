#pragma once

#include <array>
#include <cstdint>
#include <functional> // std::hash 特化（BlockPos 作 unordered_map key 用）

namespace engine::shared {

// 方块类型（原型最小集：先有 4 种，后续按需扩充）。
enum class BlockType : std::uint8_t {
    Air = 0,   // 空气（无方块）
    Stone,     // 石头
    Wood,      // 木头
    Dirt,      // 泥土
};

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
// 只存 BlockType（1 字节/方块，4096 字节/chunk = 恰好一页内存）：
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

// 一个 chunk 的方块数据：16³ 密集数组（默认全 Air=0）。
struct ChunkData {
    std::array<std::uint8_t, CHUNK_VOLUME> blocks{}; // 索引=(z&15)*256+(y&15)*16+(x&15)
    std::uint32_t dirty = 0;                          // 变更原因位掩码（0=干净）

    // 世界坐标访问（内部换算块内偏移；负数由 &15 正确落到块内 0..15）。
    std::uint8_t atWorld(int x, int y, int z) const {
        return blocks[(z & 15) * 256 + (y & 15) * 16 + (x & 15)];
    }
    std::uint8_t& atWorld(int x, int y, int z) {
        return blocks[(z & 15) * 256 + (y & 15) * 16 + (x & 15)];
    }
    std::uint8_t atWorld(const BlockPos& p) const { return atWorld(p.x, p.y, p.z); }
    std::uint8_t& atWorld(const BlockPos& p) { return atWorld(p.x, p.y, p.z); }

    void markDirty(std::uint32_t reason) { dirty |= reason; }
    void clearDirty()                     { dirty = 0; }
};

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

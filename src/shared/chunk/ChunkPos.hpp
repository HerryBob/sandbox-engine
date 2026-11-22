#pragma once
// B30：这里只为用 BlockPos 而 include 了整个 Block.hpp（连带 BlockType / PaletteEntry /
// Block 的定义与 std::hash 特化）。编译耦合偏高，将来应把 BlockPos 拆成独立的
// BlockPos.hpp，Block.hpp 与 ChunkPos.hpp 都只依赖它。
#include "shared/block/Block.hpp"

#include <cstddef>

namespace engine::shared {

// 区块边长：每轴 16 个方块，构成 16³ 立方体。
constexpr int CHUNK_SIZE   = 16;
// 单个区块的方块总数（16³ = 4096）。
constexpr int CHUNK_VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

// 区块坐标 = 世界坐标 >> 4，每个 chunk 覆盖 [pos*16, pos*16+15]。
struct ChunkPos {
    int x = 0;
    int y = 0;
    int z = 0;

    // 手写逐成员比较（B28：C++20 可直接 = default）。
    bool operator==(const ChunkPos& rhs) const = default;
};

// 世界坐标 → 区块坐标（算术右移向下取整：-1 属于 chunk -1，块内 15）。
// 这里 >> 4 强绑定区块大小
inline ChunkPos toChunkOf(const BlockPos& p) {
    return ChunkPos{p.x >> 4, p.y >> 4, p.z >> 4};
}

} // namespace engine::shared

// std::hash 特化：让 ChunkPos 可作 unordered_map 的 key（坐标映射）。
namespace std {
template <>
struct hash<engine::shared::ChunkPos> {
    std::size_t operator()(const engine::shared::ChunkPos& p) const noexcept {
        // 与 BlockPos 同款混合散列（区块坐标也是三维整数坐标）。
        size_t h = static_cast<size_t>(p.x);
        h = h * 31 + static_cast<size_t>(p.y);
        h = h * 31 + static_cast<size_t>(p.z);
        return h;
    }
};
} // namespace std

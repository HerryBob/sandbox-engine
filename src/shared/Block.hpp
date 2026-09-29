#pragma once

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

    // C++20 默认比较：自动生成 ==/!=（坐标映射 key 需要）。
    bool operator==(const BlockPos&) const = default;
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

} // namespace engine::shared

// std::hash 特化：让 BlockPos 可作 unordered_map 的 key（坐标映射）。
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
} // namespace std

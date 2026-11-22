#pragma once
// 面剔除（face culling）：只生成"暴露在空气 / 越界邻居"的面。
//
// 面掩码 = 位图（bitmap）：bit0..bit5 对应 kFaceN 顺序 {+X,-X,+Y,-Y,+Z,-Z}，1=生成该面。
// 一个 uint8_t 存 6 个开关——比 6 个 bool 省空间、可整体传参、可整体位运算。
//
// 用法：
//   1) 建 solid 集合（所有非空气方块位置）；
//   2) 循环里 computeFaceMask(block.pos, solid) 查 6 邻居，得到掩码；
//   3) buildCubeVertices(center, faceMask) 只生成掩码置位的面。
//
// 性能：6 次 O(1) 哈希查询/方块；越界坐标不在集合 → 视为空气（世界边界天然暴露，无需特判）。
//
// 设计：本文件只放"邻居判定"（函数边界解耦），不建独立检验层/掩码表——
// 掩码是一次性中间产物（256 方块/帧，用完即弃），物化无收益。
// 未来扩展（AO 环境遮蔽、光照）需要更多邻居采样时，扩展本文件的函数签名即可；
// 只有当判定结果需要跨帧复用/多处消费时，才升级为缓存（Sodium 的 ShapeComparisonCache 同理）。

#include <cstdint>
#include <unordered_set>

#include "shared/block/Block.hpp"

namespace engine::render::optimization::meshing {

// 邻居方向表：下标 f 与 kFaceN[6]（VulkanMesh.cpp）/ 掩码 bit 一一对齐（+X -X +Y -Y +Z -Z）。
inline constexpr int kNeighbor[6][3] = {
    { 1, 0, 0}, {-1, 0, 0},   // +X, -X
    { 0, 1, 0}, { 0,-1, 0},   // +Y, -Y
    { 0, 0, 1}, { 0, 0,-1}    // +Z, -Z
};

// 计算单个方块的面掩码：邻居是实心 → 该面被遮挡 → 位清 0；邻居非实心/越界 → 置 1。
// solid：所有非空气方块位置集合（主世界坐标；刚体内方块不进 meshing）。
inline std::uint8_t computeFaceMask(
    const engine::shared::BlockPos& pos,
    const std::unordered_set<engine::shared::BlockPos>& solid) noexcept {
    std::uint8_t mask = 0;
    for (int f = 0; f < 6; ++f) {
        const engine::shared::BlockPos nb{
            pos.x + kNeighbor[f][0],    
            pos.y + kNeighbor[f][1],
            pos.z + kNeighbor[f][2] };
        if (!solid.contains(nb)) 
            mask |= static_cast<std::uint8_t>(1u << f);
    }
    return mask;
}

} // namespace engine::render::meshing

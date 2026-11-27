#pragma once
#include <algorithm>
#include <climits>
#include <cstddef>

#include <glm/glm.hpp>

#include "shared/block/Block.hpp"

namespace engine::shared {

// ============================================================================
// 结构（刚体）的局部坐标系约定 —— **唯一实现**，物理与渲染都必须用这一份
// ============================================================================
//
// 背景（B141）：方块占据 [p, p+1]，所以"方块中心"= p + 0.5。
// 打包时如果直接用整数格点当原点，形状/网格又按 p+0.5 摆放，
// 等价于把原点放在**包围盒的角**上 —— 平移看不出来，一旋转就绕角转。
//
// 本文件把三件事定死，避免两边各写一遍算错：
//   origin  = (min + max) / 2          整数格点（局部坐标保持整数，可作 map key）
//   center  = (min + max + 1) / 2      几何中心（**可能半整数**）
//   bias    = 质心 - origin             把"局部方块中心"的整体质心平移到原点（COM 恒为 0）
//
// 于是三处换算统一为：
//   刚体位姿          body.pos      = origin + bias          （世界坐标 = 几何中心）
//   局部方块中心      p + 0.5 - bias （物理形状偏移、渲染网格顶点都用这个）
//   解包回世界坐标    world = body.pos + (p + 0.5 - bias) - 0.5
//                            = body.pos + p - bias
//
// 例（3×3×3，方块坐标 −1..1）：origin=0、center=0.5、bias=0.5
//     局部方块中心 = p + 0.5 − 0.5 = p ∈ {−1,0,1}（对称、以中心为 0）
// 例（2 个方块，坐标 0..1）：origin=0、center=1.0、bias=1.0
//     局部方块中心 = p + 0.5 − 1.0 ∈ {−0.5, +0.5}（对称）
// 注意 bias **不是常数**（取决于尺寸奇偶），所以必须按结构现算，不能写死 0.5。

// 单轴：整数基准
inline int structureOriginAxis(int mn, int mx) { return (mn + mx) / 2; }
// 单轴：几何中心（可能半整数）
inline double structureCenterAxis(int mn, int mx) {
    return (static_cast<double>(mn) + static_cast<double>(mx) + 1.0) * 0.5;
}
// 单轴：bias = 几何中心 - 整数基准
inline double structureBiasAxis(int mn, int mx) {
    return structureCenterAxis(mn, mx) - static_cast<double>(structureOriginAxis(mn, mx));
}

// 由方块列表（**局部坐标**）算 bias = 质心。
//
// 为什么用质心而不是包围盒中心（2026-11-27 修"每次放/拆方块刚体抖一下"）：
//   Jolt 的 Body::SetShapeInternal → UpdateCenterOfMassInternal 会在换形状时
//   `mPosition += R * (新COM − 旧COM)` —— 它自己会挪一次体来保持几何不动。
//   如果形状的 COM 不为 0，Jolt 挪一次、我们再补偿一次 → 净位移 = 新COM → 抖动。
//   取 bias = 质心 ⇒ 每个子形状的偏移均值恒为 0 ⇒ 形状 COM 恒为 0 ⇒ Jolt 不动，
//   我们只补偿 (新偏置 − 旧偏置)，几何正好保持原地。
//   注意：规整矩形（3×3×3 等）两者的值相同（都是 0.5），所以既有用例的期望值不变；
//   只有"编辑后变成不规则形状"时才会出现差异 —— 那正是抖动出现的时机。
template <typename BlockRange>
glm::dvec3 structureBias(const BlockRange& blocks) {
    double sx = 0.0, sy = 0.0, sz = 0.0;
    std::size_t n = 0;
    for (const auto& b : blocks) {
        sx += static_cast<double>(b.pos.x);
        sy += static_cast<double>(b.pos.y);
        sz += static_cast<double>(b.pos.z);
        ++n;
    }
    if (n == 0) return glm::dvec3(0.0);   // 空集合：无偏移
    // 方块中心 = p + 0.5，所以质心 = 平均(p) + 0.5
    const double inv = 1.0 / static_cast<double>(n);
    return glm::dvec3(sx * inv + 0.5, sy * inv + 0.5, sz * inv + 0.5);
}

// 打包时用：方块是世界坐标，先把整数基准减掉再算（与局部坐标的 bias 完全等价）。
template <typename BlockRange>
glm::dvec3 structureBiasWorld(const BlockRange& blocks, int originX, int originY, int originZ) {
    double sx = 0.0, sy = 0.0, sz = 0.0;
    std::size_t n = 0;
    for (const auto& b : blocks) {
        sx += static_cast<double>(b.pos.x - originX);
        sy += static_cast<double>(b.pos.y - originY);
        sz += static_cast<double>(b.pos.z - originZ);
        ++n;
    }
    if (n == 0) return glm::dvec3(0.0);
    const double inv = 1.0 / static_cast<double>(n);
    return glm::dvec3(sx * inv + 0.5, sy * inv + 0.5, sz * inv + 0.5);
}

// 局部方块中心（相对几何中心）：p + 0.5 - bias。
// 物理形状偏移与渲染网格顶点都必须用这个值。
inline glm::vec3 localBlockCenter(const BlockPos& p, const glm::dvec3& bias) {
    return glm::vec3(static_cast<float>(static_cast<double>(p.x) + 0.5 - bias.x),
                     static_cast<float>(static_cast<double>(p.y) + 0.5 - bias.y),
                     static_cast<float>(static_cast<double>(p.z) + 0.5 - bias.z));
}

} // namespace engine::shared

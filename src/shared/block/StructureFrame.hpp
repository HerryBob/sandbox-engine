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
//   bias    = center - origin          把"局部方块中心"整体平移到以几何中心为 0
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

// 由方块列表（世界坐标或局部坐标的**同一套**）算 bias。
// 模板：物理侧的 Block 与渲染侧的 Block 是同一个类型，两边调用同一份代码。
template <typename BlockRange>
glm::dvec3 structureBias(const BlockRange& blocks) {
    int mnx = INT_MAX, mny = INT_MAX, mnz = INT_MAX;
    int mxx = INT_MIN, mxy = INT_MIN, mxz = INT_MIN;
    for (const auto& b : blocks) {
        mnx = std::min(mnx, b.pos.x); mxx = std::max(mxx, b.pos.x);
        mny = std::min(mny, b.pos.y); mxy = std::max(mxy, b.pos.y);
        mnz = std::min(mnz, b.pos.z); mxz = std::max(mxz, b.pos.z);
    }
    if (mnx > mxx) return glm::dvec3(0.0);   // 空集合：无偏移
    return glm::dvec3(structureBiasAxis(mnx, mxx),
                      structureBiasAxis(mny, mxy),
                      structureBiasAxis(mnz, mxz));
}

// 局部方块中心（相对几何中心）：p + 0.5 - bias。
// 物理形状偏移与渲染网格顶点都必须用这个值。
inline glm::vec3 localBlockCenter(const BlockPos& p, const glm::dvec3& bias) {
    return glm::vec3(static_cast<float>(static_cast<double>(p.x) + 0.5 - bias.x),
                     static_cast<float>(static_cast<double>(p.y) + 0.5 - bias.y),
                     static_cast<float>(static_cast<double>(p.z) + 0.5 - bias.z));
}

} // namespace engine::shared

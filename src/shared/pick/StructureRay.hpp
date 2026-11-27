#pragma once
#include <cmath>
#include <cstdint>

#include "shared/pick/PickRay.hpp"

namespace engine::shared::pick {

// ============================================================================
// 结构（刚体）局部坐标下的体素射线
// ============================================================================
//
// 结构 = 一组"单位立方体"，局部坐标就是整数格：方块 p 占据 [p, p+1]³。
// 这与渲染端完全一致（顶点 = p + 0.5，位姿 = 格点原点，见 PhysicsWorld::publishPoseFrame），
// 所以业务线程只要把射线变到结构局部空间，就能用**和世界方块同一套 DDA**求交。
//
// 为什么放在 shared 且是纯函数：这是"业务侧同步裁决"的核心几何，必须能脱离线程/
// Vulkan/Jolt 单独验证（见 core/SelfTest.cpp 的 structureRay 用例）。
//
// 参数：
//   ox/oy/oz, dx/dy/dz —— 射线（**已在结构局部空间**，方向需单位化；实现里不归一化）
//   maxDist            —— 局部空间的最大距离（旋转不改变长度，所以等于世界距离）
//   hasBlock(x,y,z)    —— 该局部格是否有方块（业务侧用镜像的哈希表）
// 返回：PickHit（kind=Block 表示命中；这里用 Body 语义由调用方填，kind 填 None/Body 由调用方决定）
struct LocalHit {
    bool   hit = false;
    double t = 1e30;
    int    x = 0, y = 0, z = 0;   // 命中的局部格
    int    nx = 0, ny = 0, nz = 0; // 命中面法线（局部单位轴，指向射线来的一侧）
};

template <typename HasBlock>
LocalHit raycastLocalVoxel(double ox, double oy, double oz,
                           double dx, double dy, double dz,
                           double maxDist, HasBlock&& hasBlock) {
    LocalHit out;

    const double fx = std::floor(ox), fy = std::floor(oy), fz = std::floor(oz);
    int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);

    // 起点格直接命中（射线起点在实体内）
    if (hasBlock(ix, iy, iz)) {
        out.hit = true; out.t = 0.0;
        out.x = ix; out.y = iy; out.z = iz;
        return out;
    }

    const double tDeltaX = (dx != 0.0) ? std::abs(1.0 / dx) : 1e30;
    const double tDeltaY = (dy != 0.0) ? std::abs(1.0 / dy) : 1e30;
    const double tDeltaZ = (dz != 0.0) ? std::abs(1.0 / dz) : 1e30;
    const int stepX = (dx > 0.0) ? 1 : (dx < 0.0 ? -1 : 0);
    const int stepY = (dy > 0.0) ? 1 : (dy < 0.0 ? -1 : 0);
    const int stepZ = (dz > 0.0) ? 1 : (dz < 0.0 ? -1 : 0);

    double tMaxX = (stepX > 0) ? (fx + 1.0 - ox) / dx : ((stepX < 0) ? (fx - ox) / dx : 1e30);
    double tMaxY = (stepY > 0) ? (fy + 1.0 - oy) / dy : ((stepY < 0) ? (fy - oy) / dy : 1e30);
    double tMaxZ = (stepZ > 0) ? (fz + 1.0 - oz) / dz : ((stepZ < 0) ? (fz - oz) / dz : 1e30);

    // 步数上限：maxDist 格最多穿过 ~3*maxDist 个格（对角线），留余量
    const int maxSteps = static_cast<int>(maxDist * 3.0) + 4;
    for (int s = 0; s < maxSteps; ++s) {
        double tEntry = 0.0;
        int nx = 0, ny = 0, nz = 0;
        if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
            tEntry = tMaxX; tMaxX += tDeltaX;
            ix += stepX; nx = -stepX;
        } else if (tMaxY <= tMaxZ) {
            tEntry = tMaxY; tMaxY += tDeltaY;
            iy += stepY; ny = -stepY;
        } else {
            tEntry = tMaxZ; tMaxZ += tDeltaZ;
            iz += stepZ; nz = -stepZ;
        }
        if (tEntry > maxDist) break;
        if (hasBlock(ix, iy, iz)) {
            out.hit = true; out.t = tEntry;
            out.x = ix; out.y = iy; out.z = iz;
            out.nx = nx; out.ny = ny; out.nz = nz;
            return out;
        }
    }
    return out;   // 未命中：t 保持 1e30（裁决时视为"更远"）
}

} // namespace engine::shared::pick

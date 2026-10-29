#pragma once
// 方块网格构建：24 顶点（每面 4 顶点独立）+ 36 索引（每面 2 三角形）。
// 与 VulkanContext 解耦的自由函数；Vertex 布局见 VulkanContext.hpp。
#include "VulkanContext.hpp"
#include <cstdint>
namespace engine::render::mesh {

// 方块几何：24 顶点 + 36 索引（每面独立 4 顶点、独立颜色）。
// offset：方块中心的世界坐标偏移（本阶段用于摆放多个方块）。
// faceMask：位 0~5 = +X -X +Y -Y +Z -Z（1=画该面，0=剔除）。
// atlasUV：图集 UV 矩形 {u0, v0, u1, v1}——该方块纹理在图集中的区域。
// 颜色责任归纹理（片元仅采样纹理）；顶点色固定白色占位调制因子。
std::vector<Vertex> buildCubeVertices(const glm::vec3& offset, std::uint8_t faceMask,
                                      const glm::vec4& atlasUV);

} // namespace engine::render::mesh

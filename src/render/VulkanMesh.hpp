#pragma once
// 方块网格构建：24 顶点（每面 4 顶点独立）+ 36 索引（每面 2 三角形）。
// 与 VulkanContext 解耦的自由函数；Vertex 布局见 VulkanContext.hpp。
#include "VulkanContext.hpp"
#include <cstdint>
#include <optional>
namespace engine::render::mesh {

// 方块几何：24 顶点 + 36 索引（每面独立 4 顶点、独立颜色）。
// offset：方块中心的世界坐标偏移（本阶段用于摆放多个方块）。
// tint：可选整体色调（如玻璃淡蓝）；不传 = 用默认逐面色。
std::vector<Vertex> buildCubeVertices(const glm::vec3& offset, std::uint8_t faceMask,
                                      const std::optional<glm::vec3>& tint = std::nullopt);

} // namespace engine::render::mesh

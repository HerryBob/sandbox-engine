#pragma once
// 方块网格构建：24 顶点（每面 4 顶点独立）+ 36 索引（每面 2 三角形）。
// 与 VulkanContext 解耦的自由函数；Vertex 布局见 VulkanContext.hpp。
#include "VulkanContext.hpp"

namespace engine::render::mesh {

// 方块几何：24 顶点 + 36 索引（每面独立 4 顶点、独立颜色）。
// offset：方块中心的世界坐标偏移（本阶段用于摆放多个方块）。
std::vector<Vertex> buildCubeVertices(const glm::vec3& offset = {0.0f, 0.0f, 0.0f});

std::vector<uint32_t> buildCubeIndices();

} // namespace engine::render::mesh

#pragma once
// 方块网格构建：顶点 + 索引（每面 4 顶点独立）。
// 与 VulkanContext 解耦的自由函数；Vertex 布局见 VulkanContext.hpp。
// 两条生成路径：buildCubeVertices（硬编码立方体，兜底）/ buildModelVertices（数据驱动模型）。
#include "VulkanContext.hpp"
#include "render/assets/ModelDef.hpp"
#include "render/assets/TextureAtlas.hpp"
#include <cstdint>
namespace engine::render::mesh {

// 方块几何（硬编码）：24 顶点 + 36 索引（每面独立 4 顶点、独立颜色）。
// offset：方块中心的世界坐标偏移（本阶段用于摆放多个方块）。
// faceMask：位 0~5 = +X -X +Y -Y +Z -Z（1=画该面，0=剔除）。
// atlasUV：图集 UV 矩形 {u0, v0, u1, v1}——该方块纹理在图集中的区域。
// 颜色责任归纹理（片元仅采样纹理）；顶点色固定白色占位调制因子。
std::vector<Vertex> buildCubeVertices(const glm::vec3& offset, std::uint8_t faceMask,
                                      const glm::vec4& atlasUV);

// 方块几何（数据驱动）：按模型定义生成顶点（支持多元素、逐面纹理与剔除）。
// offset：方块中心的世界坐标偏移。
// model：模型定义（elements + textures 变量表，16 单位坐标）。
// occlusionFaceMask：与 buildCubeVertices 同语义——1=该方向可见（画），0=被邻块挡住。
//                     模型面 cullFace=d → 位 (d-1) 为 0 则剔（邻块不透明挡住）；
//                     cullFace=0（永不剔）→ 恒画。
// atlas：图集（面纹理名 → 格 → UV；未注册 → fallbackUV）。
std::vector<Vertex> buildModelVertices(const glm::vec3& offset,
                                       const engine::assets::ModelDef& model,
                                       std::uint8_t occlusionFaceMask,
                                       const engine::assets::TextureAtlas& atlas);

} // namespace engine::render::mesh

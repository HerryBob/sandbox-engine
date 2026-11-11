#pragma once
// 实体网格构建：按骨骼切分顶点，每骨骼一组（渲染时逐骨骼 draw）。
// 顶点在骨骼的模型局部空间——世界变换由渲染侧逐骨骼算，本层只管几何。
#include "VulkanContext.hpp"
#include "render/assets/EntityModel.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::render::mesh {

// 一根骨骼的网格：4 顶点/面、6 索引用两三角拼一面。
struct BoneMesh {
    std::vector<Vertex>        vertices;
    std::vector<std::uint32_t> indices;
};

// 一个实体的网格：骨骼顺序与 EntityModel::bones 一一对应。
struct EntityMeshData {
    std::vector<BoneMesh> bones;
};

// 生成实体全部骨骼的顶点/索引（16 单位 → 世界单位，原点在脚底中心）。
// model：已归一化的实体模型。
EntityMeshData buildEntityVertices(const engine::assets::EntityModel& model);

} // namespace engine::render::mesh

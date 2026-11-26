#pragma once
#include <cstdint>

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <vector>

namespace engine::render {

// ============================================================================
// 网格的公共类型（Vertex / ChunkMesh）
// ============================================================================
//
// 从 VulkanContext.hpp 抽出来，因为**多个存储类**都要用它：
//   ChunkMeshStore（世界 chunk）、BodyMeshStore（刚体）、将来的 EntityMesh
// 放在 VulkanContext.hpp 里会导致"存储类 include 渲染上下文"的循环依赖。

// 体素顶点：位置 + 颜色 + UV。布局必须与管线顶点输入描述（binding/attribute）一致。
struct Vertex {
    glm::vec3 pos;   // 方块局部坐标（-0.5..0.5 单位立方体）
    glm::vec3 color; // 顶点颜色（逐面着色，替代纹理前的遗留字段）
    glm::vec2 uv;    // 纹理坐标（图集）
};

// 一个 chunk（或一个刚体）的网格：GPU 资源 + 计数。
// 分桶：Solid（不透明）+ Cutout（镂空/透明），生命周期原子化（不拆两个 map）。
struct ChunkMesh {
    VkBuffer       vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer       indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t       vertexCount = 0;
    uint32_t       indexCount = 0;
    // Cutout 桶（镂空/透明方块，如玻璃）：独立一组 GPU 资源 + 计数
    VkBuffer       cutoutVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cutoutVertexMemory = VK_NULL_HANDLE;
    VkBuffer       cutoutIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cutoutIndexMemory = VK_NULL_HANDLE;
    uint32_t       cutoutVertexCount = 0;
    uint32_t       cutoutIndexCount = 0;
};

// 一个 chunk 的**CPU 侧**网格数据（构建器的输出、存储的输入）。
// Solid 桶 = 不透明；Cutout 桶 = 镂空/透明（玻璃等）。两组分别上传。
struct ChunkMeshData {
    std::vector<Vertex>    verts;
    std::vector<std::uint32_t> indices;
    std::vector<Vertex>    cutoutVerts;
    std::vector<std::uint32_t> cutoutIndices;

    bool empty() const { return verts.empty() && cutoutVerts.empty(); }
};

// 销毁一个网格的两组缓冲（Solid + Cutout）并归零。
// 是 free function 而不是成员：ChunkMeshStore 与 BodyMeshStore 共用同一实现。
void destroyMeshBuffers(VkDevice device, ChunkMesh& m);

} // namespace engine::render

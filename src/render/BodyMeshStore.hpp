#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "render/MeshTypes.hpp"
#include "shared/block/Block.hpp"

namespace engine::assets {
class TextureAtlas;
}

namespace engine::render {

// ============================================================================
// BodyMeshStore：刚体网格的资源表（可动方块组的 GPU 缓冲 + 位姿）
// ============================================================================
//
// 从 VulkanContext 抽出的第五块职责（S6a）。它只管**数据与资源**：
//   · upload(id, blocks)：把方块列表转成局部坐标顶点（mesh::buildBodyVertices）并上传
//   · destroy(id)：销毁该刚体的缓冲并清掉位姿
//   · setPose(id, ...)：位姿由 PoseHistory 插值好后每帧喂进来（本类不插值）
//   · meshes() / pose()：给录制命令用（只读）
//
// 为什么位姿和网格放一起：刚体的位姿与网格同生命周期（打包建、解包销），
// 分开会让"销毁网格后位姿还在"的问题散落在两处。
//
// ---- 未来接缝（不实现，只留位置）----
//   · 物理结构 LOD：upload() 会先按 LOD 简化 blocks 再构网格；本类接口不变。
//   · 多网格刚体（一个结构拆几个 draw）：meshes_ 的 value 改成 vector<ChunkMesh>。
class BodyMeshStore {
public:
    BodyMeshStore() = default;

    bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              const engine::assets::TextureAtlas& atlas);
    void shutdown();

    // 上传/替换某刚体的网格（同 id 已有则先销毁旧缓冲）。
    bool upload(int contraptionId, const std::vector<engine::shared::Block>& blocks);
    void destroy(int contraptionId);

    // 位姿（由 PoseHistory 插值后每帧喂入；本类不做插值）
    void setPose(int contraptionId, double x, double y, double z,
                 float qx, float qy, float qz, float qw);
    bool pose(int contraptionId, glm::dvec3& outPos, glm::quat& outRot) const;

    // 只读访问（录制命令用）
    const std::unordered_map<int, ChunkMesh>& meshes() const { return meshes_; }

private:
    VkDevice                   device_ = VK_NULL_HANDLE;
    VkPhysicalDevice           physicalDevice_ = VK_NULL_HANDLE;
    const engine::assets::TextureAtlas* atlas_ = nullptr;

    std::unordered_map<int, ChunkMesh>  meshes_;
    std::unordered_map<int, glm::dvec3> pos_;
    std::unordered_map<int, glm::quat>  rot_;
};

} // namespace engine::render

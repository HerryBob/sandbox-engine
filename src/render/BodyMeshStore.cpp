#include "render/BodyMeshStore.hpp"

#include <string>

#include "core/Log.hpp"
#include "render/assets/TextureAtlas.hpp"
#include "render/VulkanMesh.hpp"
#include "render/utils/CommandUtils.hpp"
#include "render/utils/ResourcesUtils.hpp"

namespace engine::render {

void destroyMeshBuffers(VkDevice device, ChunkMesh& m) {
    if (m.vertexBuffer) vkDestroyBuffer(device, m.vertexBuffer, nullptr);
    if (m.vertexMemory) vkFreeMemory(device, m.vertexMemory, nullptr);
    if (m.indexBuffer)  vkDestroyBuffer(device, m.indexBuffer, nullptr);
    if (m.indexMemory)  vkFreeMemory(device, m.indexMemory, nullptr);
    if (m.cutoutVertexBuffer) vkDestroyBuffer(device, m.cutoutVertexBuffer, nullptr);
    if (m.cutoutVertexMemory) vkFreeMemory(device, m.cutoutVertexMemory, nullptr);
    if (m.cutoutIndexBuffer)  vkDestroyBuffer(device, m.cutoutIndexBuffer, nullptr);
    if (m.cutoutIndexMemory)  vkFreeMemory(device, m.cutoutIndexMemory, nullptr);
    m = {};
}

bool BodyMeshStore::init(VkDevice device, VkPhysicalDevice physicalDevice,
                         const engine::assets::TextureAtlas& atlas) {
    device_ = device;
    physicalDevice_ = physicalDevice;
    atlas_ = &atlas;
    return true;
}

void BodyMeshStore::retire(ChunkMesh&& m) {
    graveyard_[currentSlot_ % kMaxFramesInFlight].push_back(std::move(m));
}

void BodyMeshStore::collectRetired(std::size_t slot) {
    auto& bucket = graveyard_[slot % kMaxFramesInFlight];
    for (auto& m : bucket) destroyMeshBuffers(device_, m);
    bucket.clear();
}

void BodyMeshStore::shutdown() {
    for (auto& bucket : graveyard_) {
        for (auto& m : bucket) destroyMeshBuffers(device_, m);
        bucket.clear();
    }
    for (auto& [id, m] : meshes_) destroyMeshBuffers(device_, m);
    meshes_.clear();
    pos_.clear();
    rot_.clear();
}

bool BodyMeshStore::upload(int contraptionId, const std::vector<engine::shared::Block>& blocks) {
    // 同 id 已有网格：结构变了 → 旧缓冲**延迟**销毁（可能还在飞，见头文件说明）
    auto it = meshes_.find(contraptionId);
    if (it != meshes_.end()) {
        retire(std::move(it->second));
        meshes_.erase(it);
    }
    if (atlas_ == nullptr) {
        // 防御：本类必须先 init()（拿到 device/atlas）。漏了 init 时上传会静默失败，
        // 表现为"刚体不渲染但碰撞箱还在"（S6a 首次接入就踩了这个）。
        ENGINE_LOG_ERROR("Vulkan", "BodyMeshStore 未初始化就 upload（检查 VulkanContext::init 里的 bodies_.init）");
        return false;
    }

    // 局部坐标生成顶点（不查世界邻居）；图集与注册表由资源侧提供
    const mesh::BodyMeshData data = mesh::buildBodyVertices(blocks, *atlas_);
    if (data.vertices.empty() || data.indices.empty())
        return true;   // 空刚体：不建缓冲

    ChunkMesh m;
    if (!util::createBuffer(device_, physicalDevice_, sizeof(Vertex) * data.vertices.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        m.vertexBuffer, m.vertexMemory)) return false;
    util::mapAndCopy(device_, m.vertexMemory, sizeof(Vertex) * data.vertices.size(), data.vertices.data());

    if (!util::createBuffer(device_, physicalDevice_, sizeof(std::uint32_t) * data.indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        m.indexBuffer, m.indexMemory)) return false;
    util::mapAndCopy(device_, m.indexMemory, sizeof(std::uint32_t) * data.indices.size(), data.indices.data());

    m.vertexCount = static_cast<std::uint32_t>(data.vertices.size());
    m.indexCount  = static_cast<std::uint32_t>(data.indices.size());
    meshes_[contraptionId] = m;

    ENGINE_LOG_INFO("Vulkan", "刚体网格 id=" + std::to_string(contraptionId)
        + " 顶点=" + std::to_string(m.vertexCount)
        + " 索引=" + std::to_string(m.indexCount));
    return true;
}

void BodyMeshStore::destroy(int contraptionId) {
    auto it = meshes_.find(contraptionId);
    if (it != meshes_.end()) {
        retire(std::move(it->second));   // 延迟销毁：同理
        meshes_.erase(it);
    }
    pos_.erase(contraptionId);
    rot_.erase(contraptionId);
}

void BodyMeshStore::setPose(int contraptionId, double x, double y, double z,
                            float qx, float qy, float qz, float qw) {
    pos_[contraptionId] = glm::dvec3{x, y, z};
    rot_[contraptionId] = glm::quat{qw, qx, qy, qz};   // glm::quat 构造顺序是 (w,x,y,z)
}

bool BodyMeshStore::pose(int contraptionId, glm::dvec3& outPos, glm::quat& outRot) const {
    const auto pit = pos_.find(contraptionId);
    const auto rit = rot_.find(contraptionId);
    if (pit == pos_.end() || rit == rot_.end()) return false;
    outPos = pit->second;
    outRot = rit->second;
    return true;
}

} // namespace engine::render

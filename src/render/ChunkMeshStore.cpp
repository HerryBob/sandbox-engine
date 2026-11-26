#include "render/ChunkMeshStore.hpp"

#include <sstream>

#include "core/Log.hpp"
#include "render/utils/CommandUtils.hpp"
#include "render/utils/ResourcesUtils.hpp"

namespace engine::render {

bool ChunkMeshStore::init(VkDevice device, VkPhysicalDevice physicalDevice) {
    device_ = device;
    physicalDevice_ = physicalDevice;
    return true;
}

void ChunkMeshStore::shutdown() {
    for (auto& [cp, m] : meshes_) destroyMeshBuffers(device_, m);
    meshes_.clear();
    for (auto& bucket : graveyard_) {
        for (auto& m : bucket) destroyMeshBuffers(device_, m);
        bucket.clear();
    }
}

void ChunkMeshStore::retire(ChunkMesh&& m) {
    graveyard_[currentSlot_ % kMaxFramesInFlight].push_back(std::move(m));
}

void ChunkMeshStore::collectRetired(std::size_t slot) {
    auto& bucket = graveyard_[slot % kMaxFramesInFlight];
    for (auto& m : bucket) destroyMeshBuffers(device_, m);
    bucket.clear();
}

bool ChunkMeshStore::upload(const engine::shared::ChunkPos& cp, const ChunkMeshData& data) {
    // 无面 → 删除旧网格（地形挖空/卸载），延迟销毁不等 GPU
    auto it = meshes_.find(cp);
    if (data.empty()) {
        if (it != meshes_.end()) {
            retire(std::move(it->second));
            meshes_.erase(it);
        }
        return true;
    }
    if (it != meshes_.end()) {
        retire(std::move(it->second));
        meshes_.erase(it);
    }

    ChunkMesh m;
    // Solid 桶
    if (!util::createBuffer(device_, physicalDevice_, sizeof(Vertex) * data.verts.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        m.vertexBuffer, m.vertexMemory)) return false;
    util::mapAndCopy(device_, m.vertexMemory, sizeof(Vertex) * data.verts.size(), data.verts.data());
    if (!util::createBuffer(device_, physicalDevice_, sizeof(uint32_t) * data.indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        m.indexBuffer, m.indexMemory)) return false;
    util::mapAndCopy(device_, m.indexMemory, sizeof(uint32_t) * data.indices.size(), data.indices.data());
    m.vertexCount = static_cast<uint32_t>(data.verts.size());
    m.indexCount  = static_cast<uint32_t>(data.indices.size());

    // Cutout 桶（无镂空面则保持 NULL 句柄）
    if (!data.cutoutVerts.empty()) {
        if (!util::createBuffer(device_, physicalDevice_, sizeof(Vertex) * data.cutoutVerts.size(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m.cutoutVertexBuffer, m.cutoutVertexMemory)) return false;
        util::mapAndCopy(device_, m.cutoutVertexMemory,
                         sizeof(Vertex) * data.cutoutVerts.size(), data.cutoutVerts.data());
        if (!util::createBuffer(device_, physicalDevice_, sizeof(uint32_t) * data.cutoutIndices.size(),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m.cutoutIndexBuffer, m.cutoutIndexMemory)) return false;
        util::mapAndCopy(device_, m.cutoutIndexMemory,
                         sizeof(uint32_t) * data.cutoutIndices.size(), data.cutoutIndices.data());
        m.cutoutVertexCount = static_cast<uint32_t>(data.cutoutVerts.size());
        m.cutoutIndexCount  = static_cast<uint32_t>(data.cutoutIndices.size());
    }
    meshes_[cp] = m;

    std::ostringstream os;
    os << "重建 chunk=(" << cp.x << "," << cp.y << "," << cp.z
       << ") 面=" << (m.indexCount + m.cutoutIndexCount) / 6
       << " 顶点=" << (m.vertexCount + m.cutoutVertexCount);
    ENGINE_LOG_INFO("Vulkan", os.str());
    return true;
}

} // namespace engine::render

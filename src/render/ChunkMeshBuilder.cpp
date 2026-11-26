#include "render/ChunkMeshBuilder.hpp"

#include <unordered_map>

#include "render/assets/ModelRegistry.hpp"
#include "render/assets/TextureAtlas.hpp"
#include "render/meshing/FaceCulling.hpp"
#include "render/VulkanMesh.hpp"
#include "registry/BlockRegistry.hpp"

namespace engine::render {

void ChunkMeshBuilder::buildSolidSet(const std::vector<engine::shared::Block>& blocks,
                                     std::unordered_set<engine::shared::BlockPos>& out) {
    out.clear();
    out.reserve(blocks.size());
    for (const auto& b : blocks) {
        if (b.contraptionId != -1) continue;   // 刚体方块不进静态集合
        const auto* info = engine::registry::g_blockRegistry.find(
            static_cast<std::uint16_t>(b.type));
        if (info && info->renderType == engine::registry::RenderType::Solid) {
            out.insert(b.pos);
        }
    }
}

ChunkMeshData ChunkMeshBuilder::buildChunk(const std::vector<engine::shared::Block>& chunkBlocks,
                                           const std::unordered_set<engine::shared::BlockPos>& solid,
                                           const engine::assets::TextureAtlas& atlas) {
    ChunkMeshData data;
    uint32_t vertexOffset = 0;
    uint32_t cutoutVertexOffset = 0;

    // P14-2a：渲染属性只取决于方块**类型**，本次重建内按类型缓存一次，
    // 之后每块 1 次哈希（原来每块查 3 次表，首帧 82,944 块烧 63ms）。
    struct BlockRenderProps {
        bool  isCutout = false;
        glm::vec4 uvRect{0.0f};
        const engine::assets::ModelDef* model = nullptr;
    };
    std::unordered_map<std::uint16_t, BlockRenderProps> renderPropsCache;
    renderPropsCache.reserve(16);

    for (const auto& block : chunkBlocks) {
        const glm::vec3 center{block.pos.x + 0.5f, block.pos.y + 0.5f, block.pos.z + 0.5f};
        const auto cacheSlot =
            renderPropsCache.try_emplace(static_cast<std::uint16_t>(block.type));
        if (cacheSlot.second) {
            BlockRenderProps props;
            const auto* info = engine::registry::g_blockRegistry.find(
                static_cast<std::uint16_t>(block.type));
            props.isCutout = info && info->renderType == engine::registry::RenderType::Cutout;
            props.uvRect = atlas.fallbackUV();
            if (info && !info->texture.empty()) {
                engine::assets::AtlasSlot slot;
                if (atlas.slotOf(info->texture, slot)) props.uvRect = atlas.slotUV(slot);
            }
            if (info && !info->model.empty())
                props.model = engine::assets::g_modelRegistry.find(info->model);
            cacheSlot.first->second = props;
        }
        const BlockRenderProps& props = cacheSlot.first->second;

        const bool isCutout = props.isCutout;
        auto& dstVerts   = isCutout ? data.cutoutVerts : data.verts;
        auto& dstIndices = isCutout ? data.cutoutIndices : data.indices;
        auto& dstOffset  = isCutout ? cutoutVertexOffset : vertexOffset;

        const std::uint8_t faceMask = optimization::meshing::computeFaceMask(block.pos, solid);
        // 顶点生成：优先数据驱动模型；无 model / 模型缺失 → 立方体兜底（仍用注册表纹理）
        const std::size_t vertsBefore = dstVerts.size();
        if (props.model)
            mesh::buildModelVertices(dstVerts, center, *props.model, faceMask, atlas);
        if (dstVerts.size() == vertsBefore)
            mesh::buildCubeVertices(dstVerts, center, faceMask, props.uvRect);

        const uint32_t faceCount = static_cast<uint32_t>((dstVerts.size() - vertsBefore) / 4);
        for (uint32_t f = 0; f < faceCount; ++f) {
            const uint32_t b0 = dstOffset + f * 4;
            dstIndices.push_back(b0 + 0); dstIndices.push_back(b0 + 1); dstIndices.push_back(b0 + 2);
            dstIndices.push_back(b0 + 0); dstIndices.push_back(b0 + 2); dstIndices.push_back(b0 + 3);
        }
        dstOffset += static_cast<uint32_t>(dstVerts.size() - vertsBefore);
    }
    return data;
}

} // namespace engine::render
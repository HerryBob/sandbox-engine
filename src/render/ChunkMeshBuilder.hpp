#pragma once
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "render/MeshTypes.hpp"
#include "shared/block/Block.hpp"

namespace engine::assets {
class TextureAtlas;
}

namespace engine::render {

// ============================================================================
// ChunkMeshBuilder：方块快照 → 顶点/索引（**纯 CPU，零 Vulkan 依赖**）
// ============================================================================
//
// 从 VulkanContext::rebuildChunkMeshes 里剥出的生成部分（S5）。剥出来的理由：
//   ① 可单测：给 8 个方块 → 断言顶点/索引数，不需要跑引擎
//   ② 可挪线程：以后异步构网格（工作线程）或 LOD（按 lod 生成简化几何）
//   ③ 与上传解耦：ChunkMeshStore 只管缓冲，本类只管算
//
// 面剔除需要一个"不透明方块坐标集"（solid）：调用方**一次建好**，多个 chunk 共用，
// 否则每 chunk 重扫全部方块（原实现就是每 chunk 内重扫，P9 记过这个开销）。
//
// ---- 未来接缝（不实现，只留位置）----
//   · 世界 LOD：buildChunk(..., int lod) —— 输出顶点数随 lod 变化；现在恒为最高精度。
//   · 异步构网格：本类无状态、纯函数，可直接在工作线程调用，结果交给 Store 上传。
class ChunkMeshBuilder {
public:
    // 面剔除用的不透明方块坐标集（一次建好，供多个 chunk 共用）。
    static void buildSolidSet(const std::vector<engine::shared::Block>& blocks,
                              std::unordered_set<engine::shared::BlockPos>& out);

    // 生成一个 chunk 的网格数据（输入必须已按该 chunk 过滤过）。
    static ChunkMeshData buildChunk(const std::vector<engine::shared::Block>& chunkBlocks,
                                    const std::unordered_set<engine::shared::BlockPos>& solid,
                                    const engine::assets::TextureAtlas& atlas);
};

} // namespace engine::render

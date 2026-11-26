#pragma once
#include <chrono>
#include <cstddef>
#include <deque>
#include <unordered_set>
#include <vector>

#include "shared/block/Block.hpp"
#include "shared/chunk/ChunkPos.hpp"

namespace engine::render {

class VulkanContext;

// ============================================================================
// ChunkMeshScheduler：脏 chunk 的网格重建排队（P20）
// ============================================================================
//
// 问题：原来渲染端一次把**所有**脏 chunk 都重建。流式加载时一批就是 4~16 个，
// 每个约 2.4~4.5ms（含面剔除与顶点生成）→ 那一帧 10~70ms，玩家看到"区块更新时卡一下"。
//
// 做法：请求进来排成 job，每帧只取**一批**去做：
//   - 个数上限 maxChunks（默认 2）
//   - 时间预算 budgetMs（默认 4ms，约 6ms 帧预算的 2/3）
// 剩下的留到下一帧，视觉上是"地形分几帧长出来"，而不是整帧卡死。
//
// **每个 job 自带方块快照，绝不共用/覆盖**（这是本类的关键正确性前提）：
//   曾经把"最新请求的快照"存成一个成员并覆写，于是先到的 job 用后到 job 的快照重建 ——
//   那种快照里没有这些 chunk 的方块，结果整片区块被建成空网格（表现为"大量区块空洞"）。
//   现在 job = {blocks 快照, 待做 chunk 集合}，任一批 chunk 只用它自己那份快照。
//   代价是每个待办 job 各留一份快照（流式加载时通常 1~3 个 job，几十 KB 级）。
//
// ---- 未来接缝（现在不实现，只留位置）----
//   · 世界 LOD：job 里的 chunk 键会扩成 (chunkPos, lod)；重建时按 lod 调用
//     ChunkMeshBuilder::build(blocks, lod)。现在恒为最高精度、键只有 chunkPos。
//   · 异步构网格：drain() 的"取一批 → 调 vk.rebuildChunkMeshes"可以直接换成
//     "投递给工作线程"，返回 future，由 drawFrame 取已完成批次上传（本类接口不变）。
class ChunkMeshScheduler {
public:
    ChunkMeshScheduler() = default;

    // 请求重建（一次流式批次 / 一次方块变更为一个 job）。
    // blocks 必须是 dirty ∪ 邻居的快照（面剔除要用邻居方块）。
    void request(std::vector<engine::shared::Block> blocks,
                 const std::unordered_set<engine::shared::ChunkPos>& dirty);

    // 每帧调一次（drawFrame 之前）：按预算取一批重建，剩下的留到下一帧。
    // 返回本帧实际重建的 chunk 数。
    std::size_t drain(VulkanContext& vk);

    bool        idle() const { return jobs_.empty(); }
    std::size_t pendingCount() const;   // 待重建 chunk 总数（诊断用）

    // 预算参数（逻辑层调参，不是世界状态）
    void setBudget(double ms, int maxChunks) { budgetMs_ = ms; maxChunks_ = maxChunks; }

private:
    struct Job {
        std::vector<engine::shared::Block>           blocks;   // 该 job 的定格快照
        std::unordered_set<engine::shared::ChunkPos> chunks;   // 该 job 还没做的 chunk
    };
    std::deque<Job> jobs_;
    // 兜底：job 溢出时的一次性请求（整体做完，不排队）
    std::vector<engine::shared::Block>           immediate_;
    std::unordered_set<engine::shared::ChunkPos> immediateChunks_;
    // 每帧只重建 1 个 chunk：实测单 chunk 的"建剔除集合 + 顶点生成 + 4 组缓冲上传"
    // 就要 3~7ms，两个就会撞穿 6ms 帧预算（表现为流式加载时的偶发卡顿）。
    double budgetMs_ = 2.5;
    int    maxChunks_ = 1;
    // job 太多（流式猛加载）时不再排队，直接一次性做完新请求——
    // 宁可偶尔长一帧，也不要无限堆积快照/延迟。
    static constexpr std::size_t kMaxJobs = 8;
};

} // namespace engine::render

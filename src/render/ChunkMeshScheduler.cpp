#include "render/ChunkMeshScheduler.hpp"

#include "render/VulkanContext.hpp"

namespace engine::render {

void ChunkMeshScheduler::request(std::vector<engine::shared::Block> blocks,
                                 const std::unordered_set<engine::shared::ChunkPos>& dirty) {
    if (dirty.empty()) return;

    if (jobs_.size() >= kMaxJobs) {
        // 排队太长（流式猛加载）：直接把这次请求整体做完，避免快照无限堆积。
        immediate_ = std::move(blocks);
        immediateChunks_ = dirty;
        return;
    }
    jobs_.push_back(Job{std::move(blocks), dirty});
}

std::size_t ChunkMeshScheduler::pendingCount() const {
    std::size_t n = 0;
    for (const auto& j : jobs_) n += j.chunks.size();
    return n;
}

std::size_t ChunkMeshScheduler::drain(VulkanContext& vk) {
    std::size_t done = 0;
    const auto t0 = std::chrono::steady_clock::now();

    // 兜底队列（job 溢出时的一次性请求）：优先做完
    if (!immediateChunks_.empty()) {
        vk.rebuildChunkMeshes(immediate_, immediateChunks_);
        done += immediateChunks_.size();
        immediate_.clear();
        immediate_.shrink_to_fit();
        immediateChunks_.clear();
        return done;
    }

    while (!jobs_.empty() && static_cast<int>(done) < maxChunks_) {
        Job& j = jobs_.front();
        std::unordered_set<engine::shared::ChunkPos> batch;
        batch.reserve(static_cast<std::size_t>(maxChunks_));

        auto it = j.chunks.begin();
        while (it != j.chunks.end() && static_cast<int>(batch.size()) < maxChunks_) {
            batch.insert(*it);
            it = j.chunks.erase(it);
        }
        if (batch.empty()) { jobs_.pop_front(); continue; }

        // 关键：批次只用**这个 job 自己的**快照重建，绝不混用其它 job 的快照
        vk.rebuildChunkMeshes(j.blocks, batch);
        done += batch.size();

        if (j.chunks.empty()) jobs_.pop_front();

        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        if (ms >= budgetMs_) break;   // 超预算：剩下的下一帧继续
    }
    return done;
}

} // namespace engine::render

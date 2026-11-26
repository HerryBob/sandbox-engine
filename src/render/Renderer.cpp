#include "render/Renderer.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <sstream>
#include "business/BusinessWorld.hpp"

namespace engine::render {


void Renderer::tick() {
    // 首帧：把整个世界一次性建好网格（此后只走增量）。
    // 注意（bug记录 B137）：这里**只由首帧门控**，目前没有"世界重建"的入口 ——
    // 将来若加切世界/重载存档，必须显式重置 fullMeshBuilt_ 或改成 rebuildAll() 调用，
    // 否则世界换了而网格还是旧的。
    if (!fullMeshBuilt_) {
        fullMeshBuilt_ = true;
        vk_.rebuildMeshFromBlocks(world_.renderSnapshot());
    }
    engine::shared::BlockChangeEvent ev;
    while (changeQueue_.pop(ev)) {
        changeEventsReceived_.fetch_add(1, std::memory_order_relaxed);
        for (const auto& cp : ev.dirtyChunks) dirtyChunks_.insert(cp);
    }
    // 有脏 chunk → 每帧最多取 kMaxChunksPerTick 个做**快照 + 排队**。
    // 为什么限制在快照这一步（而不只是重建）：world_.chunkSnapshot() 要抢世界锁并
    // 拷贝几千个 Block（含邻居），实测流式加载时它单独就能吃掉 10~16ms —— 而 P20 的
    // 预算只约束"重建"，管不到"取数"。把批量限制提前到取数，tick 的 p99 才真正下来。
    // 剩下的脏 chunk 留在 dirtyChunks_ 里，下一帧继续（不丢）。
    constexpr std::size_t kMaxChunksPerTick = 1;
    if (!dirtyChunks_.empty()) {
        std::unordered_set<engine::shared::ChunkPos> batch;
        batch.reserve(kMaxChunksPerTick);
        auto it = dirtyChunks_.begin();
        while (it != dirtyChunks_.end() && batch.size() < kMaxChunksPerTick) {
            batch.insert(*it);
            it = dirtyChunks_.erase(it);
        }
        // 取数只导 batch ∪ 6 邻居（面剔除要用邻居方块）
        // 取数只导 batch ∪ 6 邻居（面剔除要用邻居方块）
        meshScheduler_.request(world_.chunkSnapshot(batch), batch);
    }
    // 在录制本帧命令之前做一批重建（旧 buffer 走"按帧槽延迟销毁"，无需等 GPU）
    meshScheduler_.drain(vk_);
    // ---- HUD 开关：F3 按下边沿（键盘回调置位，渲染线程 exchange 消费后清零）----
    if (input_.toggleHud.exchange(false, std::memory_order_relaxed)) {
        hudVisible_ = !hudVisible_;
        vk_.setHudVisible(hudVisible_);
    }

    // ---- HUD 业务统计：1Hz 喂一次 ----
    // chunkCount()/blockCount() 内部要抢 blocksMutex_（和业务线程同一个锁），
    // 每帧查等于让渲染帧率去和 25TPS 业务 tick 抢锁 —— 所以降到 1Hz。
    // 相机/FPS 不走这里：它们由 VulkanContext 自己从 CameraState / 帧计时里取。
    const auto now = std::chrono::steady_clock::now();
    const bool firstSample = (lastHudStats_.time_since_epoch().count() == 0);
    if (firstSample || now - lastHudStats_ >= std::chrono::milliseconds(1000)) {
        lastHudStats_ = now;
        vk_.setHudStats(static_cast<long long>(world_.chunkCount()),
                        static_cast<long long>(world_.blockCount()),
                        world_.tickCount(),
                        world_.simTime());
    }
}

} // namespace engine::render

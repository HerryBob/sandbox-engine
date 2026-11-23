#include "render/Renderer.hpp"
#include "business/BusinessWorld.hpp"

namespace engine::render {

void Renderer::tick() {
    if (!first_) {
        first_ = true;
        vk_.rebuildMeshFromBlocks(world_.renderSnapshot());
    }
    engine::shared::BlockChangeEvent ev;
    while (changeQueue_.pop(ev)) {
        changeEventsReceived_.fetch_add(1, std::memory_order_relaxed);
        for (const auto& cp : ev.dirtyChunks) dirtyChunks_.insert(cp);
    }
    // 有脏 chunk → 请求增量重建（取数只导 dirty ∪ 6 邻居，drawFrame 在 GPU 安全点执行）
    // ===== 临时计时（P14 测量用，测完删除）=====
    // 每 30 帧伪造一个脏 chunk，逼出一次增量重建；数据不变 → 重建出的 mesh 与原来完全一致。
    {
        static int perfProbeFrames = 0;
        if (++perfProbeFrames >= 30) {
            perfProbeFrames = 0;
            dirtyChunks_.insert(engine::shared::ChunkPos{0, 0, 0});
        }
    }
    // ===== 临时计时结束 =====
    if (!dirtyChunks_.empty()) {
        vk_.requestChunkRebuild(world_.chunkSnapshot(dirtyChunks_), dirtyChunks_);
        dirtyChunks_.clear();
    }

    // ---- HUD 开关：F3 按下边沿（输入线程置位，渲染线程 exchange 消费后清零）----
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

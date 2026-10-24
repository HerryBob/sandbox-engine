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
    // 有脏 chunk → 请求全量重建（真正销毁/重建由 drawFrame 在 GPU 安全点执行）
    if (!dirtyChunks_.empty()) {
        vk_.requestMeshRebuild(world_.renderSnapshot());
        dirtyChunks_.clear();
    }
}

} // namespace engine::render

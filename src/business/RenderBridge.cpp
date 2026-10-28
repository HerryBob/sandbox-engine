#include "business/RenderBridge.hpp"

namespace engine::business {

RenderBridge::RenderBridge() = default;

void RenderBridge::flushChanges(World& world, long long tick) {
    // 业务 Tick 边界：把本 tick 变脏的 chunk 聚合为一条变更事件发出（数据层 ② 第四项）。
    // 事件源 = World 的脏集合（动作写入，自动去重）；push 成功才清空。
    auto dirty = world.peekDirtyChunks();
    if (dirty.empty()) return;
    engine::shared::BlockChangeEvent ev;
    ev.tick = tick;
    ev.dirtyChunks.assign(dirty.begin(), dirty.end());
    if (changeEventQueue_.push(ev)) {
        world.clearDirtyChunks();
        changeEventsSent_.fetch_add(1, std::memory_order_relaxed);
    }
    // push 失败（队列满）不清空：下 tick 重试。变更事件可延迟送达，
    // 渲染端最终一致（与位置快照"满则丢"不同：丢变更 = 渲染端漏重建）。
}

engine::core::SPSCQueue<engine::shared::BlockChangeEvent>& RenderBridge::changeEventQueue() {
    return changeEventQueue_;
}

long long RenderBridge::changeEventsSent() const {
    return changeEventsSent_.load(std::memory_order_relaxed);
}

} // namespace engine::business

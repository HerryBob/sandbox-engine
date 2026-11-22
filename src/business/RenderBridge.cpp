#include "business/RenderBridge.hpp"

namespace engine::business {

RenderBridge::RenderBridge() = default;

void RenderBridge::flushChangesToRender(World& world, long long tick) {
    // 业务 Tick 边界：把本 tick 变脏的 chunk 聚合为一条变更事件发出（数据层 ② 第四项）。
    // 事件源 = World 的脏集合（动作写入，自动去重）；push 成功才清空。
    auto dirty = world.peekDirtyChunks();
    if (dirty.empty()) return;
    engine::shared::BlockChangeEvent ev;
    ev.tick = tick;
    // B107：move 而不是 assign 拷贝 —— peekDirtyChunks 已经返回一份拷贝，
    // 再 assign 就是第二次全量拷贝（脏 300 个 chunk 时每 tick 白拷两次）。
    ev.dirtyChunks.assign(std::make_move_iterator(dirty.begin()),
                          std::make_move_iterator(dirty.end()));
    if (businessToRenderChanges_.push(ev)) {
        world.clearDirtyChunks();
        sentChangeEventCount_.fetch_add(1, std::memory_order_relaxed);
    }
    // push 失败（队列满）不清空：下 tick 重试。变更事件可延迟送达，
    // 渲染端最终一致（与位置快照"满则丢"不同：丢变更 = 渲染端漏重建）。
    //
    // B108/B109（已评估，本轮不改）：脏集合的规模**有上界** —— 最多是窗口内
    // 已加载 chunk 数（81 列 × 4 层 = 324），不会无限增长；单条事件最多 324 个
    // chunk，渲染端会一次重建 324 个（大范围改动时），但这是"改多少重建多少"的
    // 正确代价。真要限流，应该由渲染端按帧预算分片重建，而不是在这里丢事件。
}

bool RenderBridge::pushBodyStructure(const engine::shared::BodyStructureEvent& ev) {
    // 结构事件是"事件流"：push 失败返回 false，由调用方下次重试（不能丢）。
    return businessToRenderStructure_.push(ev);
}

void RenderBridge::pushBodyPose(const engine::shared::BodyPose& pose) {
    // 位姿是"状态"：满则丢，下一 tick 还有更新的值。
    businessToRenderPoses_.push(pose);
}

engine::core::SPSCQueue<engine::shared::BlockChangeEvent>& RenderBridge::businessToRenderChanges() {
    return businessToRenderChanges_;
}

engine::core::SPSCQueue<engine::shared::BodyStructureEvent>& RenderBridge::businessToRenderStructure() {
    return businessToRenderStructure_;
}

engine::core::SPSCQueue<engine::shared::BodyPose>& RenderBridge::businessToRenderPoses() {
    return businessToRenderPoses_;
}

long long RenderBridge::sentChangeEventCount() const {
    return sentChangeEventCount_.load(std::memory_order_relaxed);
}

} // namespace engine::business

#pragma once
#include "business/World.hpp"
#include "core/SPSCQueue.hpp"
#include "shared/protocol/Protocol.hpp"

#include <atomic>

namespace engine::business {

// 业务 → 渲染桥（B3）：变更事件流的唯一通道。
// 持有 SPSC 队列：B3 变更事件（业务→渲染，BlockChangeEvent）。
// 渲染线程通过 changeEventQueue() 引用消费（第 4 轨只读观众）。
//
// 语义（与位置快照不同）：
//   位置是"状态"（合并，中间态丢弃）；变更事件是"事件流"（增量，不能丢）。
//   push 失败（队列满）不清脏集合 → 下 tick 重试，渲染端最终一致。
//   丢变更 = 渲染端漏重建 chunk mesh。
// 未来 B6（物理→渲染 实体位姿）也归本桥管（桥 = 渲染侧统一入口）。
class RenderBridge {
public:
    RenderBridge();

    // 业务 Tick 边界：把本 tick 变脏的 chunk 聚合为一条变更事件发出。
    // 事件源 = World 的脏集合（动作写入，自动去重）；push 成功才清空。
    void flushChanges(World& world, long long tick);

    // 渲染侧访问（装配层分发给渲染线程）。
    engine::core::SPSCQueue<engine::shared::BlockChangeEvent>& changeEventQueue();

    // 统计（main 只读）。
    long long changeEventsSent() const; // 已发变更事件数

private:
    // B3 业务→渲染（变更事件）。容量对齐原装配（256）。
    engine::core::SPSCQueue<engine::shared::BlockChangeEvent> changeEventQueue_{256};

    std::atomic<long long> changeEventsSent_{0}; // 已发变更事件数
};

} // namespace engine::business

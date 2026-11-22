#pragma once
#include "business/World.hpp"
#include "core/SPSCQueue.hpp"
#include "shared/protocol/Protocol.hpp"

#include <atomic>

namespace engine::business {

// 业务 → 渲染桥（B3 + B6）：渲染侧数据的唯一通道。
// 持有三条 SPSC 队列：B3 变更事件（BlockChangeEvent）、
// B6 刚体结构事件（BodyStructureEvent）、B6 刚体位姿（BodyPose）。
// 渲染线程通过各 queue() 引用消费（第 4 轨只读观众）。
//
// 语义差异：
//   变更事件 / 刚体结构事件 = "事件流"（增量，不能丢）；
//   刚体位姿 = "状态"（合并，中间帧丢弃）。
//   push 失败（队列满）时事件类不清空源数据 → 下 tick 重试，渲染端最终一致。
class RenderBridge {
public:
    RenderBridge();

    // 业务 Tick 边界：把本 tick 变脏的 chunk 聚合为一条变更事件发出。
    // 事件源 = World 的脏集合（动作写入，自动去重）；push 成功才清空。
    void flushChangesToRender(World& world, long long tick);

    // 刚体结构事件：push 失败返回 false，调用方保留下次重试。
    bool pushBodyStructure(const engine::shared::BodyStructureEvent& ev);
    // 刚体位姿：满则丢（旧位置无留存价值，下一 tick 还有新的）。
    void pushBodyPose(const engine::shared::BodyPose& pose);

    // 渲染侧访问（装配层分发给渲染线程）。
    engine::core::SPSCQueue<engine::shared::BlockChangeEvent>& businessToRenderChanges();
    engine::core::SPSCQueue<engine::shared::BodyStructureEvent>& businessToRenderStructure();
    engine::core::SPSCQueue<engine::shared::BodyPose>& businessToRenderPoses();

    // 统计（main 只读）。
    long long sentChangeEventCount() const; // 已发变更事件数

private:
    // B3 业务→渲染（变更事件）。容量对齐原装配（256）。
    engine::core::SPSCQueue<engine::shared::BlockChangeEvent> businessToRenderChanges_{256};
    // B6 业务→渲染（刚体结构事件 / 位姿）。刚体数量少，容量给 16 足够。
    engine::core::SPSCQueue<engine::shared::BodyStructureEvent> businessToRenderStructure_{16};
    engine::core::SPSCQueue<engine::shared::BodyPose>           businessToRenderPoses_{16};

    std::atomic<long long> sentChangeEventCount_{0}; // 已发变更事件数
};

} // namespace engine::business

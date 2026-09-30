#pragma once
#include "core/SPSCQueue.hpp"
#include "shared/Block.hpp"
#include "shared/Protocol.hpp"

#include <atomic>
#include <cstddef>
#include <unordered_set>

namespace engine::business {
class BusinessWorld; // 前向声明：渲染只调用其 const 只读接口，避免头文件环
}

namespace engine::render {

// 渲染线程（第 4 轨只读观众）——不向模拟发任何指令，只消费/读取。
// 每帧（60Hz 节奏由 ThreadLoop 控制）：
//   ① 消费变更事件流（业务→渲染增量通道，数据层 ② 第四项）→ 标记脏 chunk；
//   ② 按业务模拟时刻插值刚体位置（渲染插值基座消费端，数据层 ② 第二项）；
//   ③ 伪渲染帧（降频 1Hz，console ASCII）：渲染线程落地前的数据通路验证，
//      真实 GLFW/Vulkan 落地后此处变为：脏 chunk 重建 mesh + 烘焙顶点 + 上屏。
// 线程安全：只调用 BusinessWorld 的 const 锁内接口（renderSnapshot / sampleShipPos /
// shipX / simTime / tickCount / contraptionState）；dirtyChunks_ 仅渲染线程写、
// main 在三轨全部停止后读（无并发）。
class Renderer {
public:
    Renderer(engine::business::BusinessWorld& world,
             engine::core::SPSCQueue<engine::shared::BlockChangeEvent>& changeQueue)
        : world_(world), changeQueue_(changeQueue) {}

    // 渲染线程每帧调用（60Hz）。原型阶段不阻塞（消费+计数+降频输出）。
    void tick();

    // ---- 诊断（main 断言用；三轨停止后读，无并发）----
    long long changeEventsReceived() const {
        return changeEventsReceived_.load(std::memory_order_relaxed);
    }
    std::size_t seenChunkCount() const { return dirtyChunks_.size(); }
    bool hasSeenChunk(const engine::shared::ChunkPos& cp) const {
        return dirtyChunks_.count(cp) != 0;
    }

private:
    void renderFrame(); // 伪渲染帧（降频 1Hz，内部拉渲染快照）

    engine::business::BusinessWorld& world_;
    engine::core::SPSCQueue<engine::shared::BlockChangeEvent>& changeQueue_;
    std::unordered_set<engine::shared::ChunkPos> dirtyChunks_; // 见过的脏 chunk
    std::atomic<long long> changeEventsReceived_{0};
    int frames_{0}; // 帧计数（伪渲染降频：每 60 帧输出一帧）
};

} // namespace engine::render

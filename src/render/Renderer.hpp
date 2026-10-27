#pragma once
#include "core/SPSCQueue.hpp"
#include "shared/block/Block.hpp"
#include "shared/protocol/Protocol.hpp"
#include "VulkanContext.hpp"
#include <atomic>
#include <cstddef>
#include <unordered_set>

namespace engine::business {
class BusinessWorld; // 前向声明：渲染只调用其 const 只读接口，避免头文件环
}

namespace engine::render {


// 渲染线程（第 4 轨只读观众）——不向模拟发任何指令，只消费/读取。
// 每帧（60Hz 节奏由 ThreadLoop 控制）：
//   ① 消费变更事件流（业务→渲染增量通道）→ 标记脏 chunk → 请求 mesh 重建；
//   ② 绘制由渲染循环调 VulkanContext::drawFrame（acquire→录制→提交→呈现）。
// 线程安全：只调用 BusinessWorld 的 const 锁内接口（renderSnapshot / simTime /
// tickCount）；dirtyChunks_ 仅渲染线程写、main 在三轨全部停止后读（无并发）。
class Renderer {
public:
    Renderer(engine::business::BusinessWorld& world,
             engine::core::SPSCQueue<engine::shared::BlockChangeEvent>& changeQueue,
        engine::render::VulkanContext& vk)          // ← 新增参数
        : world_(world), changeQueue_(changeQueue), vk_(vk) {}

    // 渲染线程每帧调用（60Hz）：首帧全量重建 + 消费变更事件 → 请求重建。
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
    bool first_ = false;   // 第一帧标志：启动时全量重建一次 mesh
    engine::render::VulkanContext& vk_;
    engine::business::BusinessWorld& world_;
    engine::core::SPSCQueue<engine::shared::BlockChangeEvent>& changeQueue_;
    std::unordered_set<engine::shared::ChunkPos> dirtyChunks_; // 见过的脏 chunk
    std::atomic<long long> changeEventsReceived_{0};
};

} // namespace engine::render

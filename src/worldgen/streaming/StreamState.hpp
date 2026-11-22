#pragma once
#include "core/SPSCQueue.hpp"
#include "worldgen/streaming/GenTypes.hpp"

#include <atomic>
#include <cstddef>
#include <unordered_set>

namespace engine::worldgen {

// 生成线程的**数据层**：队列 + 窗口记账 + 统计。
//
// 分工与业务/物理线程一致（规则5「数据层/逻辑层分开」）：
//      StreamState    ≙  business::World / physics::PhysicsState  —— 容器与记账
//      ChunkStreamer  ≙  business::BusinessWorld / physics::PhysicsWorld
//                                                              —— 决策与推进
//
// **本类不含任何行为**：不决定何时请求/淘汰、不算距离、不产出列。
// 生成算法本身在 ColumnGenerator（纯计算、无状态），与这里也不耦合。
//
// 线程模型：两条队列各自单生产单消费（方向相反）；known_/pendingEvict_ 与
// 三个聚合字段**只有业务线程**访问；三个计数字段是原子（任意线程可读）。
class StreamState {
public:
    StreamState(std::size_t requestCapacity, std::size_t resultCapacity)
        : businessToGenRequests_(requestCapacity),
          genToBusinessResults_(resultCapacity) {}

    // ---- 两条队列（方向相反，各自单生产者/单消费者）----
    core::SPSCQueue<GenRequest> businessToGenRequests_;   // 业务 → 生成
    core::SPSCQueue<GenResult>  genToBusinessResults_;    // 生成 → 业务

    // ---- 窗口记账（业务线程独占）----
    // 已请求过的列：1b 起支持卸载，走出窗口的列从这里移除，
    // 玩家走回来时会重新请求生成（否则这些列"永不加载"）。
    std::unordered_set<ColPos> known_;
    // 待卸载的列（tick 收集，takeBatch 结算）
    std::unordered_set<ColPos> pendingEvict_;

    // ---- 计数（原子，任意线程可读；用于日志/诊断）----
    std::atomic<std::size_t> requestedColumnCount_{0};
    std::atomic<std::size_t> generatedColumnCount_{0};
    std::atomic<std::size_t> appliedColumnCount_{0};

    // ---- 已应用列的聚合统计（只有业务线程读写，故用普通字段）----
    std::size_t appliedBlocks_ = 0;
    int minSurfaceY_ = kGenMaxY;
    int maxSurfaceY_ = kGenMinY;
};

} // namespace engine::worldgen

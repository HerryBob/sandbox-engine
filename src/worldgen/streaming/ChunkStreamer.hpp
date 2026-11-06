#pragma once

#include "core/SPSCQueue.hpp"
#include "core/ThreadLoop.hpp"
#include "worldgen/generator/ColumnGenerator.hpp"
#include "worldgen/streaming/GenTypes.hpp"

#include <atomic>
#include <cstddef>
#include <unordered_set>

namespace engine::business {
class World;   // 前向声明：写入只经 World::applyGeneratedChunk，不展开其实现
}

namespace engine::worldgen {

// 流式区块生成器（1b）：把一个"按玩家位置动态请求 + 后台生成 + 结果回传"的闭环装进一个对象。
//
// 线程模型（8.4 条准则"线程跟着数据是否共享走"）：
//   业务线程（唯一 World 写者）       生成线程（只做纯计算，不持有 World）
//        │  push GenRequest  ────────►  │  pop → ColumnGenerator::generateColumn
//        │  ◄────────  push GenResult   │
//        │  pop → World::applyGeneratedChunk
//
// 两条 SPSC 队列方向相反、各自单生产单消费：无需互斥锁。
// 队列满的策略：请求满 → 剩下的下 tick 再推（不丢请求）；结果满 → 生成线程等业务线程消费。
class ChunkStreamer {
public:
    // 窗口半径（chunk）：4 → 9×9 = 81 列 = 324 个 16³ chunk（1b 决策点②）
    // 将来要放大窗口，只改这个常量或构造参数。
    static constexpr int kDefaultRadius = 4;
    // 请求队列 256 / 结果队列 64（1b 细节决策）
    static constexpr std::size_t kRequestCapacity = 256;
    static constexpr std::size_t kResultCapacity = 64;
    // 每 tick 最多应用 4 列 = 16 个 16³ chunk（一次锁写 64KB；25TPS 下约 1.6MB/s）
    static constexpr std::size_t kMaxApplyPerTick = 4;
    // 卸载半径 = 加载半径 + 1：留一圈滞回（hysteresis），站在窗口边界上不会反复加载/卸载。
    static constexpr int kEvictMargin = 1;

    // 一次 drain 的结果：
    //   appliedColumns = 本次真正写进 World 的列数
    //   evictedChunks  = 本次卸载掉的 chunk 数（> 0 时调用方需要重同步物理静态集合）
    struct DrainOutcome {
        std::size_t appliedColumns = 0;
        std::size_t evictedChunks = 0;
    };

    // generator 必须已 prepare()，且生命周期要覆盖 streamer（建议同处一个作用域）。
    ChunkStreamer(const ColumnGenerator& generator,
                  int radius = kDefaultRadius,
                  std::size_t requestCapacity = kRequestCapacity,
                  std::size_t resultCapacity = kResultCapacity);
    ~ChunkStreamer();

    ChunkStreamer(const ChunkStreamer&) = delete;
    ChunkStreamer& operator=(const ChunkStreamer&) = delete;

    void start();
    void stop();                 // 阻塞到生成线程退出（ThreadLoop::stop 内部 join）
    bool running() const;

    // 业务线程每 tick 调用：按玩家位置把窗口内"尚未请求过"的列入队（近的优先）。
    // playerX/playerZ 为世界坐标（可为负）：内部 floor 后 >>4 转 chunk 坐标。
    void tickUpdate(double playerX, double playerZ);

    // 业务线程每 tick 调用：最多 maxApply 列写进 world，并先结算上一 tick 积攒的淘汰列。
    // 必须由"唯一 World 写者"线程调用（内部会写 dirtyChunksThisTick_）。
    // 返回本次实际写入的列数与卸载的 chunk 数。
    DrainOutcome drainResults(engine::business::World& world,
                              std::size_t maxApply = kMaxApplyPerTick);

    GenStats stats() const;
    std::size_t knownColumns() const { return known_.size(); }

private:
    void workerLoop();

    const ColumnGenerator& generator_;
    int radius_;

    core::SPSCQueue<GenRequest> requests_;   // 业务 → 生成
    core::SPSCQueue<GenResult>  results_;    // 生成 → 业务
    core::ThreadLoop loop_{"worldgen-stream"};

    // 已请求过的列（业务线程独占）。1b 起支持卸载：走出窗口的列从 known_ 移除，
    // 这样玩家走回来时会重新请求生成（否则这些列"永不加载"）。
    std::unordered_set<ColPos> known_;
    // 待卸载的列（tickUpdate 收集，drainResults 结算；业务线程独占）
    std::unordered_set<ColPos> pendingEvict_;

    std::atomic<std::size_t> requested_{0};
    std::atomic<std::size_t> generated_{0};
    std::atomic<std::size_t> applied_{0};
    std::atomic<bool> started_{false};

    // 已应用列的聚合统计：只在 drainResults 所在线程（唯一 World 写者）读写，
    // 与后台线程的生成计数分开，避免为三个普通字段引入锁/CAS。
    std::size_t appliedBlocks_ = 0;
    int minSurfaceY_ = kGenMaxY;
    int maxSurfaceY_ = kGenMinY;
};

} // namespace engine::worldgen

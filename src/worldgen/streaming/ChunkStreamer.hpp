#pragma once

#include "core/SPSCQueue.hpp"
#include "core/ThreadLoop.hpp"
#include "shared/chunk/ChunkPos.hpp"
#include "worldgen/generator/ColumnGenerator.hpp"
#include "worldgen/streaming/GenTypes.hpp"
#include "worldgen/streaming/StreamState.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <unordered_set>
#include <vector>

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
//
// B129（已评估，本轮不改）：本对象被**两个线程共用**（业务线程 tick/takeBatch、
// 生成线程 workerLoop），归属只靠注释维持，没有编译期/运行期防护。可加
// "首个调用者即主人"的 Debug 断言，但断言本身要引入原子与线程 id 记录，
// 且真正防住它的是规则5 的线程本地化（已完成：本类已不再持有 World）。
// 现在两条队列各自单端使用，误用会立刻表现为队列断言或数据错乱，风险可接受。
class ChunkStreamer {
public:
    // 列的"外部来源"回调（存档读盘）：返回 true = 已填充 out，本次不生成。
    // 为什么用回调而不是直接依赖 save/SaveDir：
    //   ① worldgen 层保持"不碰磁盘、不碰 World"的边界（它只做纯计算 + 队列）
    //   ② 装配层（main）决定"存档在哪、要不要读"，生产/测试可替换成内存桩
    // 调用线程：**生成线程**（必须无共享可变状态；SaveDir 只有一个路径字符串，安全）。
    using ColumnLoader = std::function<bool(const ColPos&, ColumnBlocks&)>;
    // 窗口半径（chunk）：4 → 9×9 = 81 列 = 324 个 16³ chunk（1b 决策点②）
    // 将来要放大窗口，只改这个常量或构造参数。
    static constexpr int kDefaultRadius = 4;
    // 请求队列 256 / 结果队列 64（1b 细节决策）
    // B133：两者的**反压关系**：业务线程每 tick 最多消费 4 列（kMaxApplyPerTick），
    //        而结果队列容量 64 —— 业务线程连续卡顿 16 tick（640ms）以上，结果队列就会填满，
    //        此时生成线程的 push 会失败/阻塞等待。调小容量或调大每 tick 消费量前先算这条。
    static constexpr std::size_t kRequestCapacity = 256;
    static constexpr std::size_t kResultCapacity = 64;
    // 每 tick 最多取 4 列 = 16 个 16³ chunk。
    // B126：原注释写"一次锁写 64KB"，与实现不符 —— 落盘时一列调 4 次
    // applyGeneratedChunk（每层一次锁），所以每 tick 最多 16 次锁；
    // 每次锁写入的是一层 16×16×16（约 16KB 压缩后更少）。
    static constexpr std::size_t kMaxApplyPerTick = 4;
    // 卸载半径 = 加载半径 + 1：留一圈滞回（hysteresis），站在窗口边界上不会反复加载/卸载。
    static constexpr int kEvictMargin = 1;

    // 注入"先查盘，miss 再生成"的回调。必须在 start() 之前设置。
    void setColumnLoader(ColumnLoader loader) { loader_ = std::move(loader); }
    // 诊断：从磁盘加载成功 / 生成 的列数（生成线程写，只读统计）
    long long loadedColumnCount() const { return loadedColumns_.load(std::memory_order_relaxed); }
    long long generatedColumnCount() const { return generatedColumns_.load(std::memory_order_relaxed); }

    // 一批"待落地"的流式结果（业务线程取走后写入 World）。
    // 本类是线程本地化的：**不持有、不访问 World** —— 只产出数据。
    struct StreamBatch {
        // 要卸载的 chunk（一列 = kGenChunkLevels 个竖直 chunk）
        std::unordered_set<engine::shared::ChunkPos> deadChunks;
        // 已生成、可写入世界的列（按取出的先后）
        struct Column {
            ColPos pos;
            std::shared_ptr<const ColumnBlocks> blocks;
        };
        std::vector<Column> columns;

        bool empty() const { return deadChunks.empty() && columns.empty(); }
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
    void tick(double playerX, double playerZ);

    // 业务线程每 tick 调用：取走"待落地"的一批（替换已卸载的列 + 最多 maxColumns 列成品）。
    // 只做队列操作与自己状态的更新，不碰 World —— 落盘由调用方执行
    // （规则5：线程类的成员里不出现别的线程的数据对象）。
    StreamBatch takeBatch(std::size_t maxColumns = kMaxApplyPerTick);

    GenStats stats() const;
    // B134：stats() 里的聚合字段（appliedBlocks_ / minSurfaceY_ / maxSurfaceY_）是**普通成员**，
    //         只在业务线程（takeBatch 调用方）读写 —— 从 main 线程调 stats() 读它们是数据竞争。
    //         计数字段（requested/generated/applied）是原子，任意线程可读。
    std::size_t knownColumns() const { return state_.known_.size(); }

private:
    void workerLoop();

    // ---- tick 的四段（规则1 拆分）----
    // ① 淘汰：走出 radius_ + kEvictMargin 的列进 pendingEvict_，并从 known_ 摘掉
    void collectEvictions(int cx, int cz);
    // ①' 复查：又回到 radius_ 内的待淘汰列取消淘汰（B125，避免走回来时闪洞）
    void cancelReturnedEvictions(int cx, int cz);
    // ② 收集窗口内尚未请求的列（known_ 挡住已请求的）
    std::vector<ColPos> collectWindowCandidates(int cx, int cz) const;
    // ④ 入队（队列满即停；只记成功的，失败的不能记，否则永不重试）
    void enqueueRequests(const std::vector<ColPos>& candidates);

    const ColumnGenerator& generator_;
    int radius_;

    // 生成线程本体（逻辑层：线程生命周期归这里）
    core::ThreadLoop loop_{"worldgen-stream"};
    std::atomic<bool> started_{false};

    // 数据层（StreamState）：两条队列 + 窗口记账 + 统计。
    // 逻辑层只通过它读写，不直接碰容器 —— 将来把"流式窗口"换成别的实现时，
    // 只改这里的类型，tick/takeBatch 的决策逻辑不用动。
    ColumnLoader loader_;                                    // 可为空 = 全部生成
    std::atomic<long long> loadedColumns_{0};                // 存档命中计数
    std::atomic<long long> generatedColumns_{0};             // 生成计数
    StreamState state_;
};

} // namespace engine::worldgen

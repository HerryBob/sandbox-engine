#pragma once

#include "core/SPSCQueue.hpp"
#include "core/ThreadLoop.hpp"
#include "shared/chunk/ChunkPos.hpp"
#include "worldgen/generator/ColumnGenerator.hpp"
#include "worldgen/streaming/GenTypes.hpp"

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
    // 窗口半径（chunk）：4 → 9×9 = 81 列 = 324 个 16³ chunk（1b 决策点②）
    // 将来要放大窗口，只改这个常量或构造参数。
    static constexpr int kDefaultRadius = 4;
    // 请求队列 256 / 结果队列 64（1b 细节决策）
    static constexpr std::size_t kRequestCapacity = 256;
    static constexpr std::size_t kResultCapacity = 64;
    // 每 tick 最多取 4 列 = 16 个 16³ chunk。
    // B126：原注释写"一次锁写 64KB"，与实现不符 —— 落盘时一列调 4 次
    // applyGeneratedChunk（每层一次锁），所以每 tick 最多 16 次锁；
    // 每次锁写入的是一层 16×16×16（约 16KB 压缩后更少）。
    static constexpr std::size_t kMaxApplyPerTick = 4;
    // 卸载半径 = 加载半径 + 1：留一圈滞回（hysteresis），站在窗口边界上不会反复加载/卸载。
    static constexpr int kEvictMargin = 1;

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
    std::size_t knownColumns() const { return known_.size(); }

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

    core::SPSCQueue<GenRequest> businessToGenRequests_;   // 业务 → 生成
    core::SPSCQueue<GenResult>  genToBusinessResults_;    // 生成 → 业务
    core::ThreadLoop loop_{"worldgen-stream"};

    // 已请求过的列（业务线程独占）。1b 起支持卸载：走出窗口的列从 known_ 移除，
    // 这样玩家走回来时会重新请求生成（否则这些列"永不加载"）。
    std::unordered_set<ColPos> known_;
    // 待卸载的列（tick 收集，receiveResults 结算；业务线程独占）
    std::unordered_set<ColPos> pendingEvict_;

    std::atomic<std::size_t> requestedColumnCount_{0};
    std::atomic<std::size_t> generatedColumnCount_{0};
    std::atomic<std::size_t> appliedColumnCount_{0};
    std::atomic<bool> started_{false};

    // 已应用列的聚合统计：只在业务线程（takeBatch 调用方）读写，
    // 与后台线程的生成计数分开，避免为三个普通字段引入锁/CAS。
    std::size_t appliedBlocks_ = 0;
    int minSurfaceY_ = kGenMaxY;
    int maxSurfaceY_ = kGenMinY;
};

} // namespace engine::worldgen

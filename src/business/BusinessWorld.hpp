#pragma once
#include "core/SPSCQueue.hpp"
#include "core/TimeManager.hpp"
#include "shared/block/Block.hpp"
#include "shared/state/CameraState.hpp"
#include "shared/protocol/Protocol.hpp"
#include "shared/state/PlayerInput.hpp"

#include <atomic>
#include <chrono>
#include <functional> // std::function（forEachBlockInAABB 回调）
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::business {

// 业务世界（25TPS，dt=0.04s）：世界状态的唯一权威。
// 职责：推进 sim_time/tick、裁决方块存在性与类型、放置/破坏（主世界静态方块）。
// 工作模式：请求 → 裁决 → 排队指令 → syncToPhysics 在 tick 边界发出。
// 本类只提供命令式接口，事件源（玩家/脚本/后续模组）自行调用。
class BusinessWorld {
public:
    explicit BusinessWorld(engine::core::TimeManager& tm);

    // 推进一个业务 tick：simTime += 0.04 * scale（scale 来自 TimeManager::effectiveScale）。
    void advanceTick(double scale);

    // tick 边界：把本 tick 排队的指令（全量同步/放置/破坏）发给物理线程。
    void syncToPhysics(engine::core::SPSCQueue<engine::shared::PhysicsCommand>& q);

    // tick 边界：收取物理回执（位置快照/碰撞体数量）。
    void receiveFromPhysics(engine::core::SPSCQueue<engine::shared::PhysicsResult>& q);

    // tick 边界：把本 tick 变脏的 chunk 聚合为一条变更事件发给渲染端（第 4 轨只读）。
    // 渲染端消费后只重建变过的 chunk mesh，不做全量重绘。
    void flushChanges(engine::core::SPSCQueue<engine::shared::BlockChangeEvent>& q);
    long long changeEventsSent() const; // 已发变更事件数（断言/诊断）

    // ---- 命令式接口（演示/玩家/脚本事件源共用）----
    // 请求全量同步：把当前世界方块整体同步给物理（排队 SyncBlockSet，用于初始/重建）。
    bool syncAll();

    double     simTime() const;
    long long  tickCount() const;
    long long  commandsSent() const;     // 已下发指令数
    long long  resultsReceived() const;  // 已收到物理结果数
    int        blockX() const;           // 业务视角：物理回传的方块最新坐标
    int        blockY() const;
    int        blockZ() const;

    // 方块查询：主世界静态方块 → chunks_（16³ chunk，世界坐标，定位 O(1)）。
    std::size_t blockCount() const;
    // 渲染快照：锁内拷贝全量方块列表（供渲染线程安全调用）。
    std::vector<engine::shared::Block> renderSnapshot() const;
    // 按坐标查主世界方块；返回空 = 不存在。chunk 定位 O(1)。
    // 返回值而非指针：主世界方块由 chunk 现场构造（存储只存类型），
    // 值返回避免悬垂/并发写风险。
    std::optional<engine::shared::Block> findBlock(const engine::shared::BlockPos& p) const;

    // 已加载 chunk 数（空 chunk 不占内存；断言/诊断用）。
    std::size_t chunkCount() const;
    // 区域查询（跨 chunk；同步/LOD/变更广播共用）：遍历 [min, max] 内所有非空气方块，
    // 对每个调用 fn。原型按坐标遍历（区域内定位 O(1)）。
    void forEachBlockInAABB(const engine::shared::BlockPos& min,
                            const engine::shared::BlockPos& max,
                            const std::function<void(const engine::shared::BlockPos&,
                                                     engine::shared::BlockType)>& fn) const;
    // 某 chunk 的变更原因位掩码（修改跟踪诊断用；未加载返回 0）。
    std::uint32_t chunkDirtyFlags(const engine::shared::ChunkPos& cp) const;

    // ---- 方块放置/破坏（业务裁决，主世界静态方块）----
    // 裁决规则：①位置已占用 → 拒绝放置；②破坏不存在的方块 → 拒绝。
    // 裁决通过后修改方块表并自动排队 PlaceBlock/RemoveBlock 指令
    // （下次 syncToPhysics 发出）。
    bool placeBlock(const engine::shared::BlockPos& p, engine::shared::BlockType type);
    bool removeBlock(const engine::shared::BlockPos& p);
    // 玩家输入消费：DDA 射线检测 → 命中则放置/破坏。
    void processPlayerInput(const engine::shared::CameraState& cam,
                            const engine::shared::PlayerInput& clicks);
    long long playerActionsProcessed() const;  // 已处理玩家点击次数（断言/诊断）

    long long blocksPlaced() const;        // 成功放置次数
    long long blocksRemoved() const;       // 成功破坏次数
    long long placementsRejected() const;  // 放置被拒次数
    long long removalsRejected() const;    // 破坏被拒次数
    long long physicsBlockCount() const;   // 物理侧静态碰撞体数量快照（回传）

private:
    void maybeLog();

    engine::core::TimeManager& tm_;
    std::atomic<double>        simTime_{ 0.0 };
    std::atomic<long long>     ticks_{0};
    std::atomic<long long>     commandsSent_{0};
    std::atomic<long long>     resultsReceived_{0};
    std::atomic<int>           blockX_{0};
    std::atomic<int>           blockY_{0};
    std::atomic<int>           blockZ_{0};
    // 方块存储：主世界静态方块 → chunks_（16³ 密集 uint8，空 chunk 不占内存，
    // 定位 O(1)）。仅业务线程结构性修改；main 线程只读在锁内 ——blocksMutex_ 保护。
    std::unordered_map<engine::shared::ChunkPos, engine::shared::ChunkData> chunks_;
    mutable std::mutex blocksMutex_; // const 读函数也要能加锁（blockCount/renderSnapshot）

    // 变更事件流的事件源：本 tick 变脏的 chunk 集合（写操作插入，自动去重）。
    // flushChanges 在 tick 边界采集打包为 BlockChangeEvent 后清空。
    std::unordered_set<engine::shared::ChunkPos> dirtyChunksThisTick_;
    std::atomic<long long> changeEventsSent_{0}; // 已发变更事件数

    // 待发指令状态机（纯业务线程私有，无需原子）：
    bool wantSync_ = false;      // 待发全量同步指令（syncAll）

    // 放置/破坏待发指令（裁决通过后自动排队，下次 syncToPhysics 发出）：
    bool wantPlace_ = false;                  // 待发放置指令
    engine::shared::BlockPos placePos_;       // 放置坐标
    int  placeType_ = 0;                      // 放置类型（BlockType 底层值）
    bool wantRemove_ = false;                 // 待发破坏指令
    engine::shared::BlockPos removePos_;      // 破坏坐标

    long long lastProcessedClickSeq_ = 0;      // 上次已处理点击序号（去重：一帧只处理一次）
    std::atomic<long long> playerActions_{ 0 };  // 已处理玩家点击计数

    std::atomic<long long> blocksPlaced_{0};       // 成功放置（裁决通过）次数
    std::atomic<long long> blocksRemoved_{0};      // 成功破坏次数
    std::atomic<long long> placementsRejected_{0}; // 放置被拒次数
    std::atomic<long long> removalsRejected_{0};   // 破坏被拒次数
    std::atomic<long long> physicsBlockCount_{0};  // 物理碰撞体数量快照
    std::chrono::steady_clock::time_point startWall_; // 墙钟起点（tps 墙钟口径用）
    std::chrono::steady_clock::time_point lastLog_;
};

} // namespace engine::business

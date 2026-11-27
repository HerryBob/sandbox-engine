#pragma once
#include "business/PhysicsBridge.hpp"
#include "business/RenderBridge.hpp"
#include "business/World.hpp"
#include "core/TimeManager.hpp"
#include "shared/chunk/ChunkPos.hpp"
#include "shared/pick/PickRay.hpp"
#include "shared/state/CameraState.hpp"
#include "shared/state/PlayerInput.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <vector>

namespace engine::business {

// 业务世界（25TPS，dt=0.04s）：业务线程的调度器。
// 职责：推进 sim_time/tick、裁决方块存在性与类型（委托 World）、
//       编排两座桥（PhysicsBridge B1+B2、RenderBridge B3）与玩家输入消费。
// 本类只提供命令式接口，事件源（玩家/脚本/后续模组）自行调用。
// 工作模式：请求 → 委托 World 裁决 → 委托 PhysicsBridge 排队 → tick 边界发桥。
// 数据真源在 World（world_）；跨线程通道在桥（physicsBridge_/renderBridge_）；
// 本类不含任何方块存储、锁与队列。
class BusinessWorld {
public:
    BusinessWorld(engine::core::TimeManager& tm,
                  PhysicsBridge& physicsBridge,
                  RenderBridge& renderBridge);

    // 推进一个业务 tick：simTime += 0.04 * scale（scale 来自 TimeManager::scale，全局唯一）。
    void tick(double scale);

    // tick 边界：委托 PhysicsBridge 把待发指令（全量同步/放置/破坏）发给物理线程。
    void syncWorldToPhysics();

    // tick 边界：委托 PhysicsBridge 收取物理回执（位置快照/碰撞体数量）。
    void receiveResultsFromPhysics();

    // tick 边界：委托 RenderBridge 把本 tick 变脏的 chunk 聚合为一条变更事件
    // 发给渲染端（第 4 轨只读）。渲染端消费后只重建变过的 chunk mesh。
    void flushChangesToRender();
    long long sentChangeEventCount() const; // 已发变更事件数（断言/诊断）

    // ---- 命令式接口（演示/玩家/脚本事件源共用）----
    // 请求全量同步：把当前世界方块整体同步给物理（排队 SyncBlockSet，用于初始/重建）。
    // B92：与下面 requestPhysicsResync 的区别 ——
    //   syncAll()              立刻试一次，失败**直接返回 false**，重试责任在调用方；
    //   requestPhysicsResync() 只置标志，由 syncWorldToPhysics 每 tick 自动重试。
    //   想"自动重试"用后者；想"立刻知道成功与否"用前者。
    // B106：本函数目前**没有任何调用点**（世界重建都走 requestPhysicsResync）。
    bool syncAll();

    double     simTime() const;
    long long  tickCount() const;
    long long  sentCommandCount() const;     // 已下发指令数
    long long  receivedResultCount() const;  // 已收到物理结果数
    int        blockX() const;           // 业务视角：物理回传的方块最新坐标
    int        blockY() const;
    int        blockZ() const;

    // 方块查询：委托 World（数据真源；16³ chunk，世界坐标，定位 O(1)）。
    std::size_t blockCount() const;
    // 渲染快照：锁内拷贝全量方块列表（供渲染线程安全调用）。首帧/世界整体重建用。
    std::vector<engine::shared::Block> renderSnapshot() const;
    // 增量渲染快照：锁内只导"给定 chunk ∪ 6 邻居"（方块改动的增量重建用，委托人=World）。
    std::vector<engine::shared::Block> chunkSnapshot(
        const std::unordered_set<engine::shared::ChunkPos>& chunks,
        bool expandNeighbors = true) const;
    // 按坐标查主世界方块；返回空 = 不存在。chunk 定位 O(1)。
    // 返回值而非指针：主世界方块由 chunk 现场构造（存储只存类型），
    // 值返回避免悬垂/并发写风险。
    std::optional<engine::shared::Block> findBlock(const engine::shared::BlockPos& p) const;

    // 三态查询（B93/B97）：区分"未加载 / 空气 / 有方块"。
    // findBlock 把前两者都返回 nullopt —— 需要区分的调用方（如 DDA 射线）用这个，
    // 不必绕过封装去拿 World&。
    World::LookupResult lookupBlock(const engine::shared::BlockPos& p,
                                    engine::shared::BlockType& outType) const {
        return world_.lookupBlock(p, outType);
    }

    // 已加载 chunk 数（空 chunk 不占内存；断言/诊断用）。
    std::size_t chunkCount() const;

    // 装配期入口：世界数据真源（main 启动生成/世界重建用）。
    // 运行期外部不得持有引用（写路径仍只经命令式接口，保持调度边界）。
    // B90（已评估，本轮不改）：返回**非 const** 引用，与上面这句注释自相矛盾 ——
    // 拿到它就能绕过 BusinessWorld 的调度（直接 placeBlock 绕过统计与排队、
    // 直接 clearDirtyChunks 绕过 flush 时序）。正确做法是拆成
    // worldForSetup()（非 const，仅装配期）+ worldReadOnly()（const，运行期），
    // 但那会改动 main 的接线方式，属于接口变更，本轮只把矛盾写清楚。
    engine::business::World& world() { return world_; }

    // ---- 方块放置/破坏（业务裁决，主世界静态方块）----
    // 裁决规则：①位置已占用 → 拒绝放置；②破坏不存在的方块 → 拒绝。
    // 裁决通过后修改方块表并自动排队 PlaceBlock/RemoveBlock 指令
    // （下次 syncWorldToPhysics 发出）。
    // B91：**只允许业务线程调用** —— 内部会往 PhysicsBridge 的 SPSC 队列塞指令
    // （单生产者约定），从别的线程调会破坏该约定（UB）。
    bool placeBlock(const engine::shared::BlockPos& p, engine::shared::BlockType type);
    bool removeBlock(const engine::shared::BlockPos& p);

    // 玩家输入消费：逐条弹出点击事件 → DDA 射线检测 → 命中则放置/破坏。
    // clicks 不是 const：pop 会消费事件（队列是"事件流"，读即取走）。
    void handlePlayerInput(const engine::shared::CameraState& cam,
                            engine::shared::PlayerInput& clicks);

    // F9：把准星对着的那一个方块打包成刚体（仅第一人称）。
    // 只看最多 kPackRaySteps 格内的第一个非空气方块；没打到就什么都不做。
    void handlePackInFront(const engine::shared::CameraState& cam);

    // ---- 鼠标拾取：体素射线 + 刚体探测 → 裁决 → 唯一动作 ----
    // 业务侧只能看见 World 里的方块（已打包的方块已移走），物理侧没有静态方块的
    // 身份，所以一次点击要两条射线各自作答，再由 pick::resolve 裁决谁生效
    // （规则在 shared/pick/PickRay.hpp，纯逻辑，已有自检）。
    engine::shared::pick::PickHit raycastVoxel(const engine::shared::pick::PickRay& ray) const;

    // 请求下一次同步下发全量方块集（区块卸载等"批量增删"之后调用）。
    // 物理侧只认 SyncBlockSet 才能建立集合，逐条 RemoveBlock 卸载 4096 格不现实。
    // 只置标志：真正的下发与重试在 syncWorldToPhysics 里（指令槽位被占用时下 tick 再试）。
    void requestPhysicsResync();
    // 增量同步：只把变化的 chunk 及其方块发给物理（流式加载/卸载时用，替代全量重发）。
    // 队列满时退回"置 resyncPending_"，下次走全量，保证最终一致。
    void syncChunksToPhysics(std::vector<engine::shared::Block> blocks,
                             std::vector<engine::shared::ChunkPos> chunks);

    long long playerActionsProcessed() const;  // 已处理玩家点击次数（断言/诊断）
    long long placedBlockCount() const;        // 成功放置次数
    long long removedBlockCount() const;       // 成功破坏次数
    long long rejectedPlacementCount() const;  // 放置被拒次数
    long long rejectedRemovalCount() const;    // 破坏被拒次数
    long long physicsBlockCount() const;   // 物理侧静态碰撞体数量快照（回传）

private:
    void maybeLog();

    // 一次点击的"待裁决"记录：体素射线已经算完，等物理侧的刚体探测回执。
    struct PendingPick {
        std::uint32_t seq = 0;                        // 与物理回执对应
        engine::shared::pick::PickRay ray;            // 冻结的射线（两条射线共用）
        engine::shared::pick::PickHit blockHit;       // 体素结果（已算）
        bool leftButton = true;
        int  ageTicks = 0;                            // 超时用（对端慢不能丢点击）
    };
    static constexpr int kMaxPendingPicks = 8;        // 连点堆积上限
    static constexpr int kPickTimeoutTicks = 3;       // 3 tick（120ms）没回执就按"无刚体"处理

    // 物理回执到了 → 裁决 → 执行（恰好一个动作生效）
    void resolvePick(const PendingPick& pick, const engine::shared::pick::PickHit& bodyHit,
                     bool timedOut);
    // 把裁决结果落到 world / 发给物理
    void applyPick(const PendingPick& pick, const engine::shared::pick::PickAction& action);
    // 对本 tick 之前发出、仍无回执的点击做超时处理
    void expirePendingPicks();
    // 发一条 ProbeStructure（只查询）。返回 false = 队列满（调用方按"无刚体"处理）。
    bool pushProbe(const PendingPick& pick);
    // 发一条 InteractStructure（执行；探测阶段已完成）
    void sendStructureInteract(const engine::shared::pick::PickRay& ray, int action);

    std::vector<PendingPick> pendingPicks_;
    std::uint32_t            nextPickSeq_ = 1;

    // 世界（状态层）：方块数据 + 锁 + 查询 + 动作入口（World 内部加锁）。
    engine::business::World world_;
    // 桥（通信层）：跨线程数据唯一通道。由装配层（main）创建并注入，
    // 本类只负责在 tick 边界委托调用（syncWorldToPhysics/receiveResultsFromPhysics/flushChangesToRender）。
    engine::business::PhysicsBridge& physicsBridge_;
    engine::business::RenderBridge&  renderBridge_;

    engine::core::TimeManager& tm_;
    std::atomic<double>        simTime_{ 0.0 };
    std::atomic<long long>     tickCount_{0};

    std::atomic<long long> playerActions_{ 0 };  // 已处理玩家点击计数
    // B102：两个标志改成 atomic —— 语义上仍"只有业务线程写"，但 requestPhysicsResync()
    // 的调用点可能在别的线程（例如某个模组或诊断入口），普通 bool 会变成数据竞争。
    std::atomic<bool> resyncPending_{ false };   // 待重下发全量方块集
    // B96：收物理回执时复用的事件缓冲（避免每 tick 构造/销毁 vector）。
    // 只在业务线程使用，容量留在跨 tick 之间。
    std::vector<engine::shared::BodyStructureEvent> bodyEvents_;

    // 诊断（性能记录 P1）：累计拍过多少次全量世界快照（renderSnapshot）。
    // 稳态应为 0 —— 只有"待发全量同步"时才需要快照；每 tick 都拍就是 P1 描述的问题。
    long long syncSnapshotCount_ = 0;
    long long lastLogSnapshotCount_ = 0;
    std::atomic<bool> firstTick_{ true };                      // 首个业务 tick 强制全量同步：地形预热在主线程完成，业务线程无新列可触发

    std::atomic<long long> placedBlockCount_{0};       // 成功放置（裁决通过）次数
    std::atomic<long long> removedBlockCount_{0};      // 成功破坏次数
    std::atomic<long long> rejectedPlacementCount_{0}; // 放置被拒次数
    std::atomic<long long> rejectedRemovalCount_{0};   // 破坏被拒次数
    std::chrono::steady_clock::time_point startWall_; // 墙钟起点（tps 墙钟口径用）
    std::chrono::steady_clock::time_point lastLog_;
};

} // namespace engine::business

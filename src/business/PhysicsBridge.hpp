#pragma once
#include "core/SPSCQueue.hpp"
#include "shared/block/Block.hpp"
#include "shared/protocol/Protocol.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

namespace engine::business {

// 业务 ↔ 物理桥（B1+B2）：跨线程数据的唯一通道。
// 持有两条 SPSC 队列：B1 物理指令（业务→物理）、B2 回执（物理→业务），
// 以及"待发指令状态机"（业务线程私有；下一条发什么由桥决定）。
// 桥外零共享：任何跨线程方块数据必须走这里。
//
// 生产者/消费者分布（SPSC 约束）：
//   B1 businessToPhysicsCommands_：生产者=业务线程（syncWorldToPhysics），消费者=物理线程（advance）
//   B2 physicsToBusinessResults_ ：生产者=物理线程（pushResultsToBusiness），消费者=业务线程（receiveResultsFromPhysics）
// 物理线程通过 businessToPhysicsCommands()/physicsToBusinessResults() 引用读写自己的端点。
class PhysicsBridge {
public:
    PhysicsBridge();

    // ---- 业务侧（业务线程调用）----
    // 排队待发指令（裁决通过后调用；业务线程私有状态，无需原子）：
    // 全量同步（已有待发指令则返回 false，防覆盖）。
    bool pushSyncCommand();
    // 射线 × 刚体交互（PlaceBlock/RemoveBlock 的"刚体域"走这里）：
    // 物理侧权威求解并执行，回执 StructureEdited / StructureEditMiss。
    bool pushInteractCommand(const engine::shared::PhysicsCommand& cmd) {
        return businessToPhysicsCommands_.push(cmd);
    }
    // 增量同步：只推变化的 chunk 及其方块（流式加载/卸载）。队列满返回 false。
    bool pushChunksCommand(std::vector<engine::shared::Block> blocks,
                           std::vector<engine::shared::ChunkPos> chunks);
    void pushPlaceCommand(const engine::shared::BlockPos& p, int type);
    void pushRemoveCommand(const engine::shared::BlockPos& p);

    // 业务 Tick 边界：组装一条指令发出（优先级 全量同步→放置→破坏）。
    // fullSnapshot = 业务世界全量方块（SyncBlockSet 用；业务线程现场生成）。
    // push 成功才清对应待发位；失败（队列满）保留下 tick 重试（指令可延迟）。
    //
    // 性能（P1）：fullSnapshot 是**参入即求值**的 —— 调用方必须先问 hasPendingSync()，
    // 只在真要发全量同步时才现场拍快照，否则每 tick 白拷一次整个世界。
    void syncWorldToPhysics(const std::vector<engine::shared::Block>& fullSnapshot);

    // 是否有待发的全量同步指令。调用方据此决定"要不要拍全量快照"（P1）。
    bool hasPendingSync() const noexcept { return wantSync_; }

    // 业务 Tick 边界：收光回执并消化。
    // 位置快照/碰撞体数量 → 原子投影（本类自留）；
    // 刚体结构事件/位姿 → 输出参数，由调用方（BusinessWorld）转给 RenderBridge。
    // 两个输出参数都可传 nullptr（调用方不关心时）。
    void receiveResultsFromPhysics(std::vector<engine::shared::BodyStructureEvent>* bodyEvents,
                            std::optional<engine::shared::BodyPose>* bodyPose);

    // ---- 物理侧访问（装配层分发给物理线程）----
    engine::core::SPSCQueue<engine::shared::PhysicsCommand>& businessToPhysicsCommands();
    engine::core::SPSCQueue<engine::shared::PhysicsResult>&  physicsToBusinessResults();

    // ---- 统计（main 只读）----
    long long sentCommandCount() const;     // 已发出指令数
    long long receivedResultCount() const;  // 已收回执数
    int  blockX() const;                // 演示方块坐标投影（BlockMoved 回执）
    int  blockY() const;
    int  blockZ() const;
    long long physicsBlockCount() const; // 物理侧静态碰撞体数量投影（BlockSetSize 回执）

private:
    // B1 业务→物理（指令）；B2 物理→业务（回执）。容量对齐原装配（512）。
    engine::core::SPSCQueue<engine::shared::PhysicsCommand> businessToPhysicsCommands_{512};
    // 诊断：物理侧回执里"结构被编辑 / 射线未命中刚体"的累计数
    std::atomic<long long> structureEditedCount_{0};
    std::atomic<long long> lastStructureMiss_{0};
    engine::core::SPSCQueue<engine::shared::PhysicsResult>  physicsToBusinessResults_{512};

    // 待发指令状态机（业务线程私有，无需原子）：
    bool wantSync_ = false; // 待发全量同步指令（syncAll）
    bool wantPlace_ = false;                  // 待发放置指令
    engine::shared::BlockPos placePos_;       // 放置坐标
    int  placeType_ = 0;                      // 放置类型（BlockType 底层值）
    bool wantRemove_ = false;                 // 待发破坏指令
    engine::shared::BlockPos removePos_;      // 破坏坐标

    // 统计（跨线程原子；业务侧写、main 读）：
    std::atomic<long long> sentCommandCount_{0};
    std::atomic<long long> receivedResultCount_{0};
    std::atomic<int>       blockX_{0};
    std::atomic<int>       blockY_{0};
    std::atomic<int>       blockZ_{0};
    std::atomic<long long> physicsBlockCount_{0}; // 物理碰撞体数量快照
};

} // namespace engine::business

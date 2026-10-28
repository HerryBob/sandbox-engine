#pragma once
#include "core/SPSCQueue.hpp"
#include "shared/block/Block.hpp"
#include "shared/protocol/Protocol.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace engine::business {

// 业务 ↔ 物理桥（B1+B2）：跨线程数据的唯一通道。
// 持有两条 SPSC 队列：B1 物理指令（业务→物理）、B2 回执（物理→业务），
// 以及"待发指令状态机"（业务线程私有；下一条发什么由桥决定）。
// 桥外零共享：任何跨线程方块数据必须走这里。
//
// 生产者/消费者分布（SPSC 约束）：
//   B1 commandQueue_：生产者=业务线程（syncToPhysics），消费者=物理线程（advance）
//   B2 resultQueue_ ：生产者=物理线程（syncToBusiness），消费者=业务线程（receiveFromPhysics）
// 物理线程通过 commandQueue()/resultQueue() 引用读写自己的端点。
class PhysicsBridge {
public:
    PhysicsBridge();

    // ---- 业务侧（业务线程调用）----
    // 排队待发指令（裁决通过后调用；业务线程私有状态，无需原子）：
    // 全量同步（已有待发指令则返回 false，防覆盖）。
    bool queueSync();
    void queuePlace(const engine::shared::BlockPos& p, int type);
    void queueRemove(const engine::shared::BlockPos& p);

    // 业务 Tick 边界：组装一条指令发出（优先级 全量同步→放置→破坏）。
    // fullSnapshot = 业务世界全量方块（SyncBlockSet 用；业务线程现场生成）。
    // push 成功才清对应待发位；失败（队列满）保留下 tick 重试（指令可延迟）。
    void syncToPhysics(const std::vector<engine::shared::Block>& fullSnapshot);

    // 业务 Tick 边界：收光回执并消化（位置快照/碰撞体数量 → 原子投影）。
    void receiveFromPhysics();

    // ---- 物理侧访问（装配层分发给物理线程）----
    engine::core::SPSCQueue<engine::shared::PhysicsCommand>& commandQueue();
    engine::core::SPSCQueue<engine::shared::PhysicsResult>&  resultQueue();

    // ---- 统计（main 只读）----
    long long commandsSent() const;     // 已发出指令数
    long long resultsReceived() const;  // 已收回执数
    int  blockX() const;                // 演示方块坐标投影（BlockMoved 回执）
    int  blockY() const;
    int  blockZ() const;
    long long physicsBlockCount() const; // 物理侧静态碰撞体数量投影（BlockSetSize 回执）

private:
    // B1 业务→物理（指令）；B2 物理→业务（回执）。容量对齐原装配（512）。
    engine::core::SPSCQueue<engine::shared::PhysicsCommand> commandQueue_{512};
    engine::core::SPSCQueue<engine::shared::PhysicsResult>  resultQueue_{512};

    // 待发指令状态机（业务线程私有，无需原子）：
    bool wantSync_ = false; // 待发全量同步指令（syncAll）
    bool wantPlace_ = false;                  // 待发放置指令
    engine::shared::BlockPos placePos_;       // 放置坐标
    int  placeType_ = 0;                      // 放置类型（BlockType 底层值）
    bool wantRemove_ = false;                 // 待发破坏指令
    engine::shared::BlockPos removePos_;      // 破坏坐标

    // 统计（跨线程原子；业务侧写、main 读）：
    std::atomic<long long> commandsSent_{0};
    std::atomic<long long> resultsReceived_{0};
    std::atomic<int>       blockX_{0};
    std::atomic<int>       blockY_{0};
    std::atomic<int>       blockZ_{0};
    std::atomic<long long> physicsBlockCount_{0}; // 物理碰撞体数量快照
};

} // namespace engine::business

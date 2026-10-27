#pragma once
#include "core/SPSCQueue.hpp"
#include "core/TimeManager.hpp"
#include "shared/protocol/Protocol.hpp"

#include <atomic>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::physics {

// 原型最小刚体（Contraption 打包产物）。
// 位置用 double（精度设计见架构文档 2.8：复合体=基准+局部坐标）。
struct RigidBody {
    int    id = 0;              // 刚体 ID（物理分配）
    double x = 0.0, y = 0.0, z = 0.0; // 世界位置（基准）
    double vx = 0.0, vy = 0.0, vz = 0.0; // 速度（2.3 积分用）
    int    blockCount = 0;      // 打包的方块数
    // 内部方块（局部坐标 + 类型）：世界坐标 = 基准 + 局部坐标（架构 2.8）。
    // 解包时按基准偏移回写静态碰撞体集合。
    std::vector<engine::shared::Block> blocks;
    // 单 AABB 碰撞箱（架构 4.3 + Sable 增量思想）：
    // 最小包裹——半宽 = (max-min)/2 + 0.5；中心偏移 = (min+max)/2（包围盒中心相对基准）。
    // 刚体内方块增删（飞船改造）时增量重算，碰撞箱实时贴合方块组。
    // 世界 AABB = [基准+偏移−半宽, 基准+偏移+半宽]。
    double halfExtentX = 0.0, halfExtentY = 0.0, halfExtentZ = 0.0;
    double centerOffsetX = 0.0, centerOffsetY = 0.0, centerOffsetZ = 0.0;
};

// 物理世界（25TPS，dt=0.04s）。
// 用累积待处理模拟时间(accumulator)驱动，不硬绑定业务 tick 次数。
// 原型阶段：积分 + 处理业务指令
//（MoveBlock / SyncBlockSet 全量同步 / Place/RemoveBlock 放置破坏 / Contraption 打包解包）。
class PhysicsWorld {
public:
    explicit PhysicsWorld(engine::core::TimeManager& tm);

    // 每墙钟帧调用一次：accumulator += wall_dt * time_scale，
    // 然后 while(accumulator>=0.04) 执行物理 tick（处理业务指令）。
    void advance(double scale, double wallDt,
                 engine::core::SPSCQueue<engine::shared::PhysicsCommand>& fromBusiness);

    // 物理 → 业务：先发打包回执（逐条，不合并），再发位置快照（合并语义）。
    void syncToBusiness(engine::core::SPSCQueue<engine::shared::PhysicsResult>& q);

    double     simTime() const;
    long long  tickCount() const;
    long long  processedCommands() const;  // 累计处理的业务指令数
    int        blockX() const;             // 演示用"方块"的最新坐标（指令驱动）
    int        blockY() const;
    int        blockZ() const;
    long long  bodyCount() const;          // 当前刚体数（打包/解包后变化）
    long long  colliderCount() const;      // 当前静态碰撞体数量（放置/破坏/打包转移后变化）
    long long  collisions() const;         // 累计碰撞次数（刚体撞上静态方块）

    // 增量碰撞箱验证（原型单刚体演示用）：首个刚体的方块数与 AABB 参数。
    long long  bodyBlockCount() const;     // 首个刚体内方块数（0=无刚体）
    double     bodyHalfExtentX() const;    // 首个刚体 x 半宽（增量重算后）
    double     bodyCenterOffsetX() const;  // 首个刚体 x 中心偏移（相对基准）

private:
    void maybeLog();
    // 增量重算刚体 AABB：最小包裹（半宽 = (max-min)/2+0.5，偏移 = (min+max)/2）。
    // 打包时与刚体内方块增删时调用（Sable handleBlockChange 的对应物）；
    // 同时更新原子投影供 main 线程运行中采样。
    void recomputeAABB(RigidBody& b);

    // tick 银行上限：单帧最多补跑 5 个物理 tick（=200ms 模拟时间）。
    // 超过说明本帧严重落后（卡顿/掉帧），剩余累积时间直接丢弃，
    // 防止"越落后越要补、越补越落后"的螺旋死亡（spiral of death）。
    static constexpr int kMaxStepsPerFrame_ = 5;

    engine::core::TimeManager& tm_;
    double                     accumulator_{0.0};
    std::atomic<double>        simTime_{0.0};
    std::atomic<long long>     ticks_{0};
    std::atomic<long long>     processedCommands_{0};
    std::atomic<int>           blockX_{0};
    std::atomic<int>           blockY_{0};
    std::atomic<int>           blockZ_{0};
    long long                  lastSentProcessed_{0}; // 纯物理线程：上次回传时的处理数
    std::chrono::steady_clock::time_point startWall_; // 墙钟起点（tps 墙钟口径用）
    std::chrono::steady_clock::time_point lastLog_;
    // 刚体表（纯物理线程独占读写；bodyCount_ 是它的原子投影供 main 读）。
    std::vector<RigidBody>     bodies_;
    long long                  nextBodyId_{1};    // 刚体 ID 分配器
    std::atomic<long long>     bodyCount_{0};
    // 主刚体增量碰撞箱的原子投影（main 运行中采样，避免读 bodies_ 数据竞争）。
    std::atomic<long long>     bodyBlockCountProj_{0};
    std::atomic<double>        bodyHalfExtentXProj_{0.0};
    std::atomic<double>        bodyCenterOffsetXProj_{0.0};
    std::vector<engine::shared::PhysicsResult> pendingReceipts_; // 待发回执（纯物理线程）
    std::vector<engine::shared::PhysicsResult> pendingCollisions_; // 待发碰撞事件（纯物理线程）
    std::atomic<long long> collisions_{0};  // 累计碰撞次数（main 读）
    double lastPosSentSimT_ = -1.0; // 纯物理线程：上次已发位置快照的模拟时刻。
                                    // 物理循环每墙钟帧调 syncToBusiness，但模拟 tick 只
                                    // 25Hz——同一模拟时刻的位置被重复发 ~15 次（旧行为）。
                                    // 位置快照语义：只发"新模拟时刻"的位置（数据层②修正）。

    // 静态方块碰撞体集合（纯物理线程独占；架构 2.7 物理层）。
    // 放置/破坏增量维护；打包时方块转移进刚体（物理形态转移），解包时回写。
    std::unordered_map<engine::shared::BlockPos, engine::shared::BlockType> colliders_;

    // 接触集合（纯物理线程独占；Rapier Started/Stopped 语义的 Started 部分）：
    // 刚体 id → 接触中的静态方块坐标。碰撞事件只在"接触开始"（新加入集合）时发，
    // 持续重叠（Stay）不发；方块被删/离开后下一帧重叠检测自然移除（不发 Stopped，
    // 业务暂无接触结束需求）。全量同步/解包时整体清空对应记录。
    std::unordered_map<int, std::unordered_set<engine::shared::BlockPos>> contactPairs_;
};

} // namespace engine::physics

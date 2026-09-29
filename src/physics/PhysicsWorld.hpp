#pragma once
#include "core/SPSCQueue.hpp"
#include "core/TimeManager.hpp"
#include "shared/Protocol.hpp"

#include <atomic>
#include <chrono>
#include <vector>

namespace engine::physics {

// 原型最小刚体（Contraption 打包产物）。
// 位置用 double（精度设计见架构文档：复合体=基准+局部坐标）。
struct RigidBody {
    int    id = 0;              // 刚体 ID（物理分配）
    double x = 0.0, y = 0.0, z = 0.0; // 世界位置
    double vx = 0.0, vy = 0.0, vz = 0.0; // 速度（2.3 积分用）
    int    blockCount = 0;      // 打包的方块数
};

// 物理世界（25TPS，dt=0.04s）。
// 用累积待处理模拟时间(accumulator)驱动，不硬绑定业务 tick 次数。
// 原型阶段：积分占位 + 消费输入队列 + 处理业务指令（MoveBlock / Contraption 打包解包）。
class PhysicsWorld {
public:
    explicit PhysicsWorld(engine::core::TimeManager& tm);

    // 每墙钟帧调用一次：accumulator += wall_dt * time_scale，
    // 然后 while(accumulator>=0.04) 执行物理 tick（消费输入 + 处理业务指令）。
    void advance(double scale, double wallDt,
                 engine::core::SPSCQueue<engine::shared::InputEvent>& fromInput,
                 engine::core::SPSCQueue<engine::shared::PhysicsCommand>& fromBusiness);

    // 物理 → 业务：先发打包回执（逐条，不合并），再发位置快照（合并语义）。
    void syncToBusiness(engine::core::SPSCQueue<engine::shared::PhysicsResult>& q);

    double     simTime() const;
    long long  tickCount() const;
    long long  inputConsumed() const;
    long long  inputValid() const;
    long long  processedCommands() const;  // 累计处理的业务指令数
    int        blockX() const;             // 演示用"方块"的最新坐标（指令驱动）
    int        blockY() const;
    int        blockZ() const;
    long long  bodyCount() const;          // 当前刚体数（打包/解包后变化）

private:
    void maybeLog();

    // 推力加速度（格/s²，沿 x 正方向；原型只推 x 轴）。
    static constexpr double kThrust_ = 1.0;

    engine::core::TimeManager& tm_;
    double                     accumulator_{0.0};
    std::atomic<double>        simTime_{0.0};
    std::atomic<long long>     ticks_{0};
    std::atomic<long long>     inputConsumed_{0};
    std::atomic<long long>     inputValid_{0};
    std::atomic<long long>     processedCommands_{0};
    std::atomic<int>           blockX_{0};
    std::atomic<int>           blockY_{0};
    std::atomic<int>           blockZ_{0};
    long long                  lastSentProcessed_{0}; // 纯物理线程：上次回传时的处理数
    std::chrono::steady_clock::time_point lastLog_;

    // 刚体表（纯物理线程独占读写；bodyCount_ 是它的原子投影供 main 读）。
    std::vector<RigidBody>     bodies_;
    long long                  nextBodyId_{1};    // 刚体 ID 分配器
    std::atomic<long long>     bodyCount_{0};
    std::vector<engine::shared::PhysicsResult> pendingReceipts_; // 待发回执（纯物理线程）
    bool                       thrustActive_{false}; // 本 tick 推力状态（纯物理线程私有）
};

} // namespace engine::physics

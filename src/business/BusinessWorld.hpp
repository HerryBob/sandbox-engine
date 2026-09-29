#pragma once
#include "core/SPSCQueue.hpp"
#include "core/TimeManager.hpp"
#include "shared/Block.hpp"
#include "shared/Protocol.hpp"

#include <atomic>
#include <chrono>
#include <vector>

namespace engine::business {

// 业务世界（20TPS，dt=0.05s）。
// 原型阶段：维护 sim_time/tick 计数，并演示真实消息流——
// 每 tick 下发一条 MoveBlock 指令，同时收取物理回传的 BlockMoved 结果。
class BusinessWorld {
public:
    explicit BusinessWorld(engine::core::TimeManager& tm);

    // 推进一个业务 tick：simTime += 0.05 * time_scale
    void advanceTick(double scale);

    // 业务 Tick 边界：下发刚体指令到物理世界。
    void syncToPhysics(engine::core::SPSCQueue<engine::shared::PhysicsCommand>& q);

    // 业务 Tick 边界：收取物理结果（刚体位置回传）。
    void receiveFromPhysics(engine::core::SPSCQueue<engine::shared::PhysicsResult>& q);

    double     simTime() const;
    long long  tickCount() const;
    long long  commandsSent() const;     // 已下发指令数
    long long  resultsReceived() const;  // 已收到物理结果数
    int        blockX() const;           // 业务视角：物理回传的方块最新坐标
    int        blockY() const;
    int        blockZ() const;

    // 世界方块列表（原型：初始一艘 3×3×3 木头飞船，后续打包/解包的数据源）。
    const std::vector<engine::shared::Block>& blocks() const;
    std::size_t blockCount() const;
    // 按坐标查方块（找不到返回 nullptr；原型线性查找，将来换坐标映射）。
    const engine::shared::Block* findBlock(const engine::shared::BlockPos& p) const;

    // Contraption 打包状态（0=未打包 1=已打包 2=已解包，main 演示读取）。
    int contraptionState() const;
    // 业务视角：物理回传的刚体（飞船）最新世界位置。
    double shipX() const;
    double shipY() const;
    double shipZ() const;

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
    std::atomic<int>           contraptionState_{0};
    std::atomic<double>        shipX_{0.0};  // 刚体（飞船）位置快照（业务视角）
    std::atomic<double>        shipY_{0.0};
    std::atomic<double>        shipZ_{0.0};
    std::vector<engine::shared::Block> blocks_; // 构造时一次性写入，之后所有线程只读

    // 打包演示状态机（纯业务线程私有，无需原子）：
    bool wantPack_ = false;      // 待发打包命令
    int  packCount_ = 0;         // 打包方块数
    bool wantUnpack_ = false;    // 待发解包命令
    int  packedId_ = 0;          // 物理回执的刚体 ID
    std::chrono::steady_clock::time_point lastLog_;
};

} // namespace engine::business

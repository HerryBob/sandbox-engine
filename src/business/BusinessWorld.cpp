#include "business/BusinessWorld.hpp"
#include "core/Log.hpp"

#include <iostream>

namespace engine::business {

BusinessWorld::BusinessWorld(engine::core::TimeManager& tm)
    : tm_(tm), lastLog_(std::chrono::steady_clock::now()) {
    // 原型：初始世界放一艘 3×3×3 木头飞船。
    // blocks_ 在构造时一次性写入，之后所有线程只读（无并发写，无需原子）。
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            for (int z = -1; z <= 1; ++z) {
                blocks_.push_back(engine::shared::Block{
                    engine::shared::BlockType::Wood,
                    engine::shared::BlockPos{x, y, z}});
            }
        }
    }
}

void BusinessWorld::advanceTick(double scale) {
    // 20TPS：每个业务 tick 推进 dt=0.05s，受 time_scale 缩放。
    // simTime_ 是跨线程共享（main 线程读取），必须走原子读改写。
    simTime_.store(simTime_.load(std::memory_order_relaxed) + 0.05 * scale,
                   std::memory_order_relaxed);
    ticks_.fetch_add(1, std::memory_order_relaxed);

    // ---- 原型 Contraption 打包/解包演示状态机 ----
    // tick 50：发打包命令（把 27 块木头打包成刚体，初始速度 + 推力起飞）
    // tick 250：若已打包，发解包命令（拆回方块）——延后到 12.5s，
    //           让飞船飞过正常/减速/加速/域乘数/输入禁用多个演示阶段。
    const long long t = tickCount();
    if (t == 50) {
        wantPack_ = true;
        packCount_ = static_cast<int>(blockCount());
    } else if (t == 250 && contraptionState_.load(std::memory_order_relaxed) == 1) {
        wantUnpack_ = true;
    }
    maybeLog();
}

void BusinessWorld::syncToPhysics(engine::core::SPSCQueue<engine::shared::PhysicsCommand>& q) {
    // 每 tick 下发一条 MoveBlock 指令：把方块沿 x 方向推一格。
    // 原型演示：业务 → 物理的真实数据流（命令生产速率 = 20/s 墙钟，固定节奏）。
    engine::shared::PhysicsCommand cmd;
    if (wantPack_) {
        // 打包指令：把世界方块组（27 块）打包成刚体，初始速度沿 x 起飞（2.3 演示）。
        cmd.type = engine::shared::PhysicsCommand::Type::PackContraption;
        cmd.blockCount = packCount_;
        cmd.ivx = 1.0; // 业务裁决：飞船以 1 格/秒 沿 x 方向飞行
        if (q.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantPack_ = false;
        }
    } else if (wantUnpack_) {
        // 解包指令：把刚体拆回方块（目标刚体 ID 来自打包回执）。
        cmd.type = engine::shared::PhysicsCommand::Type::UnpackContraption;
        cmd.contraptionId = packedId_;
        if (q.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantUnpack_ = false;
        }
    } else {
        // 常规演示指令：移动方块。
        cmd.type = engine::shared::PhysicsCommand::Type::MoveBlock;
        cmd.dx   = 1;
        if (q.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
        }
        // push 失败（队列满）则丢弃：慢放时物理消化变慢、命令堆积，可观察水位现象。
    }
}

void BusinessWorld::receiveFromPhysics(
    engine::core::SPSCQueue<engine::shared::PhysicsResult>& q) {
    // 业务 Tick 边界：收物理结果（位置快照 + 打包回执）。
    engine::shared::PhysicsResult res;
    while (q.pop(res)) {
        resultsReceived_.fetch_add(1, std::memory_order_relaxed);
        switch (res.type) {
        case engine::shared::PhysicsResult::Type::BlockMoved:
            blockX_.store(res.x, std::memory_order_relaxed);
            blockY_.store(res.y, std::memory_order_relaxed);
            blockZ_.store(res.z, std::memory_order_relaxed);
            break;
        case engine::shared::PhysicsResult::Type::ContraptionPacked:
            packedId_ = res.contraptionId;
            contraptionState_.store(1, std::memory_order_relaxed);
            break;
        case engine::shared::PhysicsResult::Type::ContraptionUnpacked:
            contraptionState_.store(2, std::memory_order_relaxed);
            break;
        case engine::shared::PhysicsResult::Type::RigidBodyPos:
            // 刚体（飞船）位置快照：业务视角更新飞船坐标。
            shipX_.store(res.rx, std::memory_order_relaxed);
            shipY_.store(res.ry, std::memory_order_relaxed);
            shipZ_.store(res.rz, std::memory_order_relaxed);
            break;
        }
    }
}

double BusinessWorld::simTime() const {
    return simTime_.load(std::memory_order_relaxed);
}
long long BusinessWorld::tickCount() const {
    return ticks_.load(std::memory_order_relaxed);
}
long long BusinessWorld::commandsSent() const {
    return commandsSent_.load(std::memory_order_relaxed);
}
long long BusinessWorld::resultsReceived() const {
    return resultsReceived_.load(std::memory_order_relaxed);
}
int BusinessWorld::blockX() const { return blockX_.load(std::memory_order_relaxed); }
int BusinessWorld::blockY() const { return blockY_.load(std::memory_order_relaxed); }
int BusinessWorld::blockZ() const { return blockZ_.load(std::memory_order_relaxed); }

const std::vector<engine::shared::Block>& BusinessWorld::blocks() const {
    return blocks_;
}
std::size_t BusinessWorld::blockCount() const {
    return blocks_.size();
}
const engine::shared::Block* BusinessWorld::findBlock(const engine::shared::BlockPos& p) const {
    for (const auto& b : blocks_) {
        if (b.pos == p) return &b;
    }
    return nullptr;
}
int BusinessWorld::contraptionState() const {
    return contraptionState_.load(std::memory_order_relaxed);
}
double BusinessWorld::shipX() const { return shipX_.load(std::memory_order_relaxed); }
double BusinessWorld::shipY() const { return shipY_.load(std::memory_order_relaxed); }
double BusinessWorld::shipZ() const { return shipZ_.load(std::memory_order_relaxed); }

void BusinessWorld::maybeLog() {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - lastLog_).count() >= 1.0) {
        lastLog_ = now;
        const long long t = tickCount();
        const double sim = simTime_.load(std::memory_order_relaxed);
        const double tps = sim > 0.0 ? static_cast<double>(t) / sim : 0.0;
        std::lock_guard<std::mutex> lock(engine::core::g_logMutex);
        std::cout << "[业务] sim=" << sim << "s tick=" << t
                  << " 平均tps=" << tps
                  << " 世界方块=" << blockCount()
                  << " 打包状态=" << contraptionState()
                  << " 飞船=(" << shipX() << "," << shipY() << "," << shipZ() << ")"
                  << "  生效系数=" << tm_.effectiveScale(engine::core::Domain::Business) << "\n";
    }
}

} // namespace engine::business

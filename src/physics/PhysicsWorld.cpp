#include "physics/PhysicsWorld.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <iostream>

namespace engine::physics {

PhysicsWorld::PhysicsWorld(engine::core::TimeManager& tm)
    : tm_(tm), lastLog_(std::chrono::steady_clock::now()) {}

void PhysicsWorld::advance(double scale, double wallDt,
                           engine::core::SPSCQueue<engine::shared::InputEvent>& fromInput,
                           engine::core::SPSCQueue<engine::shared::PhysicsCommand>& fromBusiness) {
    // 累积待处理模拟时间（文档 2.2）：残差持续保留，自动处理非整数配比。
    accumulator_ += wallDt * scale;

    while (accumulator_ >= 0.04) { // 25TPS, dt=0.04
        // 读输入队列：施加推力（2.3.1 真实施力，不再只计数）。
        // 每个 tick 重置推力状态：只有本 tick 消费到"按住推力键"的 valid 事件才施力。
        thrustActive_ = false;
        engine::shared::InputEvent ev;
        while (fromInput.pop(ev)) {
            if (ev.valid) {
                inputValid_.fetch_add(1, std::memory_order_relaxed);
                if (ev.keyState == 1) thrustActive_ = true; // 按住 → 本 tick 开推力
            }
            inputConsumed_.fetch_add(1, std::memory_order_relaxed);
        }

        // 处理业务指令：MoveBlock → 方块移动；Pack/UnpackContraption → 刚体建/拆。
        // 物理消费速率 = 25×scale TPS；业务生产速率固定 20/s 墙钟——
        // 慢放时物理消化变慢、指令堆积（processed < sent），加速时追赶。
        engine::shared::PhysicsCommand cmd;
        while (fromBusiness.pop(cmd)) {
            switch (cmd.type) {
            case engine::shared::PhysicsCommand::Type::MoveBlock:
                blockX_.store(blockX_.load(std::memory_order_relaxed) + cmd.dx,
                              std::memory_order_relaxed);
                blockY_.store(blockY_.load(std::memory_order_relaxed) + cmd.dy,
                              std::memory_order_relaxed);
                blockZ_.store(blockZ_.load(std::memory_order_relaxed) + cmd.dz,
                              std::memory_order_relaxed);
                break;
            case engine::shared::PhysicsCommand::Type::PackContraption: {
                // 打包：新建刚体，应用业务裁决的初始速度（起飞速度）。
                RigidBody body;
                body.id = static_cast<int>(nextBodyId_++);
                body.blockCount = cmd.blockCount;
                body.vx = cmd.ivx;
                body.vy = cmd.ivy;
                body.vz = cmd.ivz;
                bodies_.push_back(body);
                bodyCount_.store(static_cast<long long>(bodies_.size()),
                                 std::memory_order_relaxed);
                // 回执：打包完成（回执必须逐条送达，不参与快照合并）。
                engine::shared::PhysicsResult r;
                r.type = engine::shared::PhysicsResult::Type::ContraptionPacked;
                r.contraptionId = body.id;
                r.blockCount = body.blockCount;
                pendingReceipts_.push_back(r);
                break;
            }
            case engine::shared::PhysicsCommand::Type::UnpackContraption: {
                // 解包：按 ID 拆除刚体（原型：找到就删，方块列表落回业务侧）。
                const auto it = std::find_if(
                    bodies_.begin(), bodies_.end(),
                    [&](const RigidBody& b) { return b.id == cmd.contraptionId; });
                const bool found = it != bodies_.end();
                if (found) bodies_.erase(it);
                bodyCount_.store(static_cast<long long>(bodies_.size()),
                                 std::memory_order_relaxed);
                engine::shared::PhysicsResult r;
                r.type = engine::shared::PhysicsResult::Type::ContraptionUnpacked;
                r.contraptionId = cmd.contraptionId;
                pendingReceipts_.push_back(r);
                break;
            }
            }
            processedCommands_.fetch_add(1, std::memory_order_relaxed);
        }

        // ---- 半隐式欧拉积分（2.3.1 推力）：v += a×dt，x += v×dt ----
        // 推力：按住推力键（输入有效）→ 沿 x 加速；松手/禁用 → 无加速度，惯性滑行。
        // dt=0.04 固定步长，加速度恒定（原型无重力/阻尼）。
        for (auto& b : bodies_) {
            const double ax = thrustActive_ ? kThrust_ : 0.0;
            b.vx += ax * 0.04;   // 先更新速度（半隐式：用新速度算位置）
            b.x  += b.vx * 0.04; // 再更新位置
            b.y  += b.vy * 0.04;
            b.z  += b.vz * 0.04;
        }

        // simTime_ 是跨线程共享（main 线程读取），必须走原子读改写。
        simTime_.store(simTime_.load(std::memory_order_relaxed) + 0.04,
                       std::memory_order_relaxed);
        accumulator_ -= 0.04;
        ticks_.fetch_add(1, std::memory_order_relaxed);
        maybeLog();
    }
}

void PhysicsWorld::syncToBusiness(
    engine::core::SPSCQueue<engine::shared::PhysicsResult>& q) {
    // ① 回执优先：打包/解包确认必须逐条送达（回执是事件，不能合并丢弃）。
    for (const auto& r : pendingReceipts_) q.push(r);
    pendingReceipts_.clear();

    // ② 刚体位置快照（合并语义）：有刚体就每帧发一条最新位置。
    // 刚体自己积分在动，不依赖命令——所以每帧发，不并入③的"命令驱动"节奏。
    // 原型只有 1 个刚体（飞船），发第一个；多刚体将来做批量快照。
    if (!bodies_.empty()) {
        const RigidBody& b = bodies_.front();
        engine::shared::PhysicsResult pos;
        pos.type = engine::shared::PhysicsResult::Type::RigidBodyPos;
        pos.contraptionId = b.id;
        pos.rx = b.x;
        pos.ry = b.y;
        pos.rz = b.z;
        pos.simTime = simTime_.load(std::memory_order_relaxed);
        q.push(pos); // 满则丢：业务消化不过来时丢弃旧快照可接受
    }

    // ③ 演示方块位置快照：只有本帧处理过新指令才发，避免每帧重复推快照塞满队列。
    const long long processed = processedCommands_.load(std::memory_order_relaxed);
    if (processed == lastSentProcessed_) return;
    lastSentProcessed_ = processed;

    engine::shared::PhysicsResult res;
    res.type = engine::shared::PhysicsResult::Type::BlockMoved;
    res.x = blockX_.load(std::memory_order_relaxed);
    res.y = blockY_.load(std::memory_order_relaxed);
    res.z = blockZ_.load(std::memory_order_relaxed);
    res.processedCommands = processed;
    res.simTime = simTime_.load(std::memory_order_relaxed);
    q.push(res); // 满则丢：极端情况下业务消化不过来，丢弃快照可接受
}

double PhysicsWorld::simTime() const {
    return simTime_.load(std::memory_order_relaxed);
}
long long PhysicsWorld::tickCount() const {
    return ticks_.load(std::memory_order_relaxed);
}
long long PhysicsWorld::inputConsumed() const {
    return inputConsumed_.load(std::memory_order_relaxed);
}
long long PhysicsWorld::inputValid() const {
    return inputValid_.load(std::memory_order_relaxed);
}
long long PhysicsWorld::processedCommands() const {
    return processedCommands_.load(std::memory_order_relaxed);
}
long long PhysicsWorld::bodyCount() const {
    return bodyCount_.load(std::memory_order_relaxed);
}
int PhysicsWorld::blockX() const { return blockX_.load(std::memory_order_relaxed); }
int PhysicsWorld::blockY() const { return blockY_.load(std::memory_order_relaxed); }
int PhysicsWorld::blockZ() const { return blockZ_.load(std::memory_order_relaxed); }

void PhysicsWorld::maybeLog() {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - lastLog_).count() >= 1.0) {
        lastLog_ = now;
        const long long t = tickCount();
        const double sim = simTime_.load(std::memory_order_relaxed);
        const double tps = sim > 0.0 ? static_cast<double>(t) / sim : 0.0;
        std::lock_guard<std::mutex> lock(engine::core::g_logMutex);
        std::cout << "[物理] sim=" << sim << "s tick=" << t
                  << " 平均tps=" << tps
                  << " 残差acc=" << accumulator_
                  << " 消费输入=" << inputConsumed()
                  << " 有效输入=" << inputValid()
                  << " 处理指令=" << processedCommands()
                  << " 刚体=" << bodyCount()
                  << " 推力=" << (thrustActive_ ? "开" : "关")
                  << " 方块=(" << blockX() << "," << blockY() << "," << blockZ() << ")"
                  << "  生效系数=" << tm_.effectiveScale(engine::core::Domain::Physics) << "\n";
    }
}

} // namespace engine::physics

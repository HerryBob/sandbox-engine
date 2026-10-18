#include "physics/PhysicsWorld.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace engine::physics {

PhysicsWorld::PhysicsWorld(engine::core::TimeManager& tm)
    : tm_(tm),
      startWall_(std::chrono::steady_clock::now()),
      lastLog_(std::chrono::steady_clock::now()) {}

void PhysicsWorld::advance(double scale, double wallDt,
                           engine::core::SPSCQueue<engine::shared::InputEvent>& fromInput,
                           engine::core::SPSCQueue<engine::shared::PhysicsCommand>& fromBusiness) {
    // 累积待处理模拟时间（文档 2.2）：残差持续保留，自动处理非整数配比。
    accumulator_ += wallDt * scale;

    // tick 银行：一帧最多补跑 kMaxStepsPerFrame_ 个 tick（防螺旋死亡），
    // 跑满仍超限 → 丢弃剩余累积时间（本帧已尽力，落后不欠账）。
    int steps = 0;
    while (accumulator_ >= 0.04 && steps < kMaxStepsPerFrame_) { // 25TPS, dt=0.04
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

        // 处理业务指令：放置/破坏/全量同步 → 静态碰撞体集合；
        // Pack/UnpackContraption → 刚体建/拆 + 碰撞体形态转移。
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
            case engine::shared::PhysicsCommand::Type::SyncBlockSet:
                // 全量同步：重建静态碰撞体集合（初始世界 / 世界重建时用）。
                // 世界重建：接触状态全部重置（旧的接触记录全部失效）。
                colliders_.clear();
                contactPairs_.clear();
                for (const auto& b : cmd.blocks) colliders_[b.pos] = b.type;
                break;
            case engine::shared::PhysicsCommand::Type::PlaceBlock: {
                // 放置：业务裁决已通过。目标域（cmd.contraptionId）：
                // -1 = 主世界（建静态碰撞体）；≥0 = 刚体（飞船改造：方块加入刚体，
                //      增量重算碰撞箱——碰撞箱实时扩张贴合新方块组）。
                const engine::shared::BlockPos p{cmd.x, cmd.y, cmd.z};
                if (cmd.contraptionId < 0) {
                    colliders_[p] = static_cast<engine::shared::BlockType>(cmd.blockType);
                } else {
                    const auto it = std::find_if(
                        bodies_.begin(), bodies_.end(),
                        [&](const RigidBody& b) { return b.id == cmd.contraptionId; });
                    if (it != bodies_.end()) {
                        it->blocks.push_back(engine::shared::Block{
                            static_cast<engine::shared::BlockType>(cmd.blockType), p});
                        it->blockCount = static_cast<int>(it->blocks.size());
                        recomputeAABB(*it); // 增量扩张：最小包裹实时贴合
                    }
                }
                break;
            }
            case engine::shared::PhysicsCommand::Type::RemoveBlock: {
                // 破坏：业务裁决已通过。目标域同上。
                const engine::shared::BlockPos p{cmd.x, cmd.y, cmd.z};
                if (cmd.contraptionId < 0) {
                    colliders_.erase(p);
                } else {
                    const auto it = std::find_if(
                        bodies_.begin(), bodies_.end(),
                        [&](const RigidBody& b) { return b.id == cmd.contraptionId; });
                    if (it != bodies_.end()) {
                        for (auto bIt = it->blocks.begin(); bIt != it->blocks.end(); ++bIt) {
                            if (bIt->pos == p) { it->blocks.erase(bIt); break; }
                        }
                        it->blockCount = static_cast<int>(it->blocks.size());
                        recomputeAABB(*it); // 增量收缩：碰撞箱实时贴合剩余方块组
                    }
                }
                break;
            }
            case engine::shared::PhysicsCommand::Type::PackContraption: {
                // 打包：新建刚体，应用业务裁决的初始速度（起飞速度）。
                // 物理形态转移（架构 2.7）：方块从静态碰撞体集合转移进刚体。
                RigidBody body;
                body.id = static_cast<int>(nextBodyId_++);
                body.blockCount = cmd.blockCount;
                body.vx = cmd.ivx;
                body.vy = cmd.ivy;
                body.vz = cmd.ivz;
                body.blocks = cmd.blocks; // 局部坐标 = 世界坐标（刚体建在原点 0）
                for (const auto& b : cmd.blocks) colliders_.erase(b.pos);
                recomputeAABB(body); // 最小包裹碰撞箱（半宽 + 中心偏移）
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
                // 解包：拆除刚体，方块按"基准+局部坐标"回写静态碰撞体集合。
                // 回执携带世界坐标方块列表：业务据此重建 blocks_（解包闭环，架构 2.7）。
                std::vector<engine::shared::Block> worldBlocks;
                const auto it = std::find_if(
                    bodies_.begin(), bodies_.end(),
                    [&](const RigidBody& b) { return b.id == cmd.contraptionId; });
                if (it != bodies_.end()) {
                    worldBlocks.reserve(it->blocks.size());
                    for (const auto& b : it->blocks) {
                        const engine::shared::BlockPos world{
                            static_cast<int>(std::llround(it->x + b.pos.x)),
                            static_cast<int>(std::llround(it->y + b.pos.y)),
                            static_cast<int>(std::llround(it->z + b.pos.z))};
                        colliders_[world] = b.type;
                        worldBlocks.push_back(engine::shared::Block{b.type, world});
                    }
                    bodies_.erase(it);
                }
                contactPairs_.erase(cmd.contraptionId); // 刚体已拆：其接触记录作废
                bodyCount_.store(static_cast<long long>(bodies_.size()),
                                 std::memory_order_relaxed);
                engine::shared::PhysicsResult r;
                r.type = engine::shared::PhysicsResult::Type::ContraptionUnpacked;
                r.contraptionId = cmd.contraptionId;
                r.blocks = std::move(worldBlocks);
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

        // ---- 单 AABB 碰撞检测与响应（架构 4.3，原型只处理 x 方向运动）----
        // 先移动后响应：刚体 AABB 与静态方块碰撞体（每格 1×1×1）相交检测。
        // 粗相思想：只查询刚体 AABB 覆盖的整格坐标（空间查询），不遍历全部碰撞体。
        // 世界 AABB = 基准 + 中心偏移 ± 半宽（增量重算的最小包裹碰撞箱）。
        // 接触语义（Rapier Started/Stopped 的 Started 部分）：碰撞响应（回退+清速）
        // 每帧可重复（顶着方块持续重叠），但事件只在"接触开始"（新加入接触集合）时发
        // 一次——同一对 (刚体, 方块) 不重复上报，业务按真实碰撞次数裁决。
        for (auto& b : bodies_) {
            const double leftX  = b.x + b.centerOffsetX - b.halfExtentX;
            const double rightX = b.x + b.centerOffsetX + b.halfExtentX;
            const double leftY  = b.y + b.centerOffsetY - b.halfExtentY;
            const double rightY = b.y + b.centerOffsetY + b.halfExtentY;
            const double leftZ  = b.z + b.centerOffsetZ - b.halfExtentZ;
            const double rightZ = b.z + b.centerOffsetZ + b.halfExtentZ;
            const int x0 = static_cast<int>(std::floor(leftX));
            const int x1 = static_cast<int>(std::floor(rightX));
            const int y0 = static_cast<int>(std::floor(leftY));
            const int y1 = static_cast<int>(std::floor(rightY));
            const int z0 = static_cast<int>(std::floor(leftZ));
            const int z1 = static_cast<int>(std::floor(rightZ));
            // 本帧接触中的方块（收集全部接触，不再 break 只取第一个）。
            std::unordered_set<engine::shared::BlockPos> currentContacts;
            double vxBefore = 0.0;
            int    nx       = 0;
            bool   responded = false;
            for (int ix = x0; ix <= x1; ++ix)
                for (int iy = y0; iy <= y1; ++iy)
                    for (int iz = z0; iz <= z1; ++iz) {
                        if (colliders_.find(engine::shared::BlockPos{ix, iy, iz}) ==
                            colliders_.end())
                            continue;
                        // 区间重叠测试（严格版：贴边不算碰撞）。
                        if (rightX > ix && leftX < ix + 1.0 &&
                            rightY > iy && leftY < iy + 1.0 &&
                            rightZ > iz && leftZ < iz + 1.0) {
                            currentContacts.insert(engine::shared::BlockPos{ix, iy, iz});
                            if (!responded) {
                                // 碰撞响应：原型只回退 x 轴（飞船沿 x 运动），速度清零。
                                // 本帧只响应第一个接触（原型行为保持，断言依赖此回退）。
                                responded = true;
                                vxBefore  = b.vx;
                                nx        = (vxBefore >= 0.0) ? -1 : 1;
                                if (b.vx >= 0.0)
                                    b.x = static_cast<double>(ix) - b.centerOffsetX -
                                          b.halfExtentX; // AABB 右边界贴方块左边界
                                else
                                    b.x = static_cast<double>(ix + 1) - b.centerOffsetX +
                                          b.halfExtentX; // AABB 左边界贴方块右边界
                                b.vx = 0.0;
                            }
                        }
                    }
            // ---- 接触集合更新（Started / Stay / 结束）----
            // 新接触（不在集合）→ 加入 + 发 CollisionEvent（业务裁决）；
            // 持续接触（已在集合）→ Stay，不发；接触结束（本帧未接触）→ 移除，不发。
            auto& set = contactPairs_[b.id];
            for (const auto& cp : currentContacts) {
                if (set.insert(cp).second) {
                    collisions_.fetch_add(1, std::memory_order_relaxed);
                    engine::shared::PhysicsResult colEv;
                    colEv.type = engine::shared::PhysicsResult::Type::CollisionEvent;
                    colEv.contraptionId = b.id;
                    colEv.cx = cp.x; colEv.cy = cp.y; colEv.cz = cp.z;
                    colEv.nx = nx;                 // 刚体被推回的方向
                    colEv.impact = std::abs(vxBefore);
                    colEv.simTime = simTime_.load(std::memory_order_relaxed);
                    pendingCollisions_.push_back(colEv);
                }
            }
            for (auto it = set.begin(); it != set.end();) {
                if (currentContacts.count(*it) == 0) it = set.erase(it);
                else ++it;
            }
        }

        // simTime_ 是跨线程共享（main 线程读取），必须走原子读改写。
        simTime_.store(simTime_.load(std::memory_order_relaxed) + 0.04,
                       std::memory_order_relaxed);
        accumulator_ -= 0.04;
        ticks_.fetch_add(1, std::memory_order_relaxed);
        ++steps;
        maybeLog();
    }

    // 超限丢时间：若跑满 kMaxStepsPerFrame_ 仍欠账（本帧严重落后），
    // 丢弃剩余累积——下一帧从零开始，不把落后带进下一帧。
    if (accumulator_ >= 0.04) {
        accumulator_ = 0.0;
    }
}

void PhysicsWorld::syncToBusiness(
    engine::core::SPSCQueue<engine::shared::PhysicsResult>& q) {
    // ① 回执优先：打包/解包确认必须逐条送达（回执是事件，不能合并丢弃）。
    for (const auto& r : pendingReceipts_) q.push(r);
    pendingReceipts_.clear();

    // ①.5 碰撞事件：事件语义，逐条不合并（业务必须逐条裁决，如破坏被撞方块）。
    for (const auto& ev : pendingCollisions_) q.push(ev);
    pendingCollisions_.clear();

    // ② 刚体位置快照（合并语义，数据层 ② 修正）：有刚体就发最新位置，
    // 但只发"新模拟时刻"——物理循环每墙钟帧调本函数，模拟 tick 却只有 25Hz，
    // 同一模拟时刻重复发会淹没业务双槽（插值窗口退化，实测 1647 条回执大量同戳）。
    // 刚体自己积分在动，不依赖命令；原型只有 1 个刚体（飞船），发第一个。
    if (!bodies_.empty()) {
        const double st = simTime_.load(std::memory_order_relaxed);
        if (st != lastPosSentSimT_) { // 模拟时刻已推进 → 发一条新位置快照
            lastPosSentSimT_ = st;
            const RigidBody& b = bodies_.front();
            engine::shared::PhysicsResult pos;
            pos.type = engine::shared::PhysicsResult::Type::RigidBodyPos;
            pos.contraptionId = b.id;
            pos.rx = b.x;
            pos.ry = b.y;
            pos.rz = b.z;
            pos.simTime = st;
            q.push(pos); // 满则丢：业务消化不过来时丢弃旧快照可接受
        }
    }

    // ③ 静态碰撞体数量快照（合并语义：业务只需最新值，用于两侧一致性核对）。
    {
        engine::shared::PhysicsResult set;
        set.type = engine::shared::PhysicsResult::Type::BlockSetSize;
        set.blockSetSize = static_cast<long long>(colliders_.size());
        set.simTime = simTime_.load(std::memory_order_relaxed);
        q.push(set); // 满则丢：快照合并语义，旧值丢弃可接受
    }

    // ④ 演示方块位置快照：只有本帧处理过新指令才发，避免每帧重复推快照塞满队列。
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
long long PhysicsWorld::colliderCount() const {
    // main 在物理线程停止后读取，colliders_ 无并发写。
    return static_cast<long long>(colliders_.size());
}
long long PhysicsWorld::collisions() const {
    return collisions_.load(std::memory_order_relaxed);
}

void PhysicsWorld::recomputeAABB(RigidBody& b) {
    // 增量重算刚体 AABB（最小包裹）：半宽 = (max-min)/2 + 0.5，中心偏移 = (min+max)/2。
    // 刚体内方块增删后调用，碰撞箱实时贴合方块组（Sable handleBlockChange 对应物）。
    // 同时更新原子投影，main 线程运行中可安全采样。
    if (b.blocks.empty()) {
        b.halfExtentX = b.halfExtentY = b.halfExtentZ = 0.0;
        b.centerOffsetX = b.centerOffsetY = b.centerOffsetZ = 0.0;
    } else {
        int minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
        for (const auto& blk : b.blocks) {
            minX = std::min(minX, blk.pos.x); maxX = std::max(maxX, blk.pos.x);
            minY = std::min(minY, blk.pos.y); maxY = std::max(maxY, blk.pos.y);
            minZ = std::min(minZ, blk.pos.z); maxZ = std::max(maxZ, blk.pos.z);
        }
        b.centerOffsetX = (minX + maxX) / 2.0;
        b.centerOffsetY = (minY + maxY) / 2.0;
        b.centerOffsetZ = (minZ + maxZ) / 2.0;
        b.halfExtentX = (maxX - minX) / 2.0 + 0.5;
        b.halfExtentY = (maxY - minY) / 2.0 + 0.5;
        b.halfExtentZ = (maxZ - minZ) / 2.0 + 0.5;
    }
    bodyBlockCountProj_.store(static_cast<long long>(b.blocks.size()),
                              std::memory_order_relaxed);
    bodyHalfExtentXProj_.store(b.halfExtentX, std::memory_order_relaxed);
    bodyCenterOffsetXProj_.store(b.centerOffsetX, std::memory_order_relaxed);
}
long long PhysicsWorld::bodyBlockCount() const {
    return bodyBlockCountProj_.load(std::memory_order_relaxed);
}
double PhysicsWorld::bodyHalfExtentX() const {
    return bodyHalfExtentXProj_.load(std::memory_order_relaxed);
}
double PhysicsWorld::bodyCenterOffsetX() const {
    return bodyCenterOffsetXProj_.load(std::memory_order_relaxed);
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
        // tps 按墙钟口径（tick / 墙钟运行秒）：模拟时间受 time_scale 缩放，
        // 用 sim 做分母在加速/减速时会失真。
        const double wall =
            std::chrono::duration<double>(now - startWall_).count();
        const double tps = wall > 0.0 ? static_cast<double>(t) / wall : 0.0;
        std::ostringstream os;
        os << "[物理] sim=" << sim << "s tick=" << t
           << " 平均tps=" << tps
           << " 残差acc=" << accumulator_
           << " 消费输入=" << inputConsumed()
           << " 有效输入=" << inputValid()
           << " 处理指令=" << processedCommands()
           << " 刚体=" << bodyCount()
           << " 碰撞体=" << colliders_.size()
           << " 碰撞=" << collisions()
           << " 推力=" << (thrustActive_ ? "开" : "关")
           << " 方块=(" << blockX() << "," << blockY() << "," << blockZ() << ")"
           << "  生效系数=" << tm_.effectiveScale(engine::core::Domain::Physics);
        engine::core::log(engine::core::LogLevel::Info, os.str());
    }
}

} // namespace engine::physics

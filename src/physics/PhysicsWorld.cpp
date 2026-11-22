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

int PhysicsWorld::addPlayerBody(double x, double y, double z,
                                double halfExtentX, double halfExtentY, double halfExtentZ,
                                double jumpSpeed) {
    // 生物体：运动学，位置锚在脚底中心。
    // 形状与跳跃初速来自实体类型定义（经装配层传入），本文件不写死某个生物的尺寸。
    // 尺寸存进 halfExtent（Jolt 接入后作为 CapsuleShape 的参数）；
    // centerOffsetY = 半高，让"基准 y = 脚底"成立。
    RigidBody b;
    b.id = state_.allocateBodyId();
    b.motion = MotionType::Kinematic;
    b.x = x;
    b.y = y;
    b.z = z;
    b.halfExtentX = halfExtentX;
    b.halfExtentY = halfExtentY;
    b.halfExtentZ = halfExtentZ;
    b.centerOffsetX = 0.0;
    b.centerOffsetY = halfExtentY; // AABB 中心在脚底上方半高，底部正好落在基准 y
    b.centerOffsetZ = 0.0;
    if (jumpSpeed > 0.0) jumpSpeed_ = jumpSpeed;
    state_.bodies().push_back(b);
    bodyCount_.store(static_cast<long long>(state_.bodies().size()), std::memory_order_relaxed);
    state_.setPlayerBodyId(b.id);
    return b.id;
}

void PhysicsWorld::attachPlayerState(engine::shared::PlayerBodyState* out) {
    playerState_ = out;
}

void PhysicsWorld::attachPlayerControl(engine::shared::PlayerControlState* in) {
    playerControl_ = in;
}

void PhysicsWorld::tick(double scale, double wallDt,
                           engine::core::SPSCQueue<engine::shared::PhysicsCommand>& fromBusiness) {
    // 累积待处理模拟时间（文档 2.2）：残差持续保留，自动处理非整数配比。
    accumulator_ += wallDt * scale;

    // tick 银行：一帧最多补跑 kMaxStepsPerFrame_ 个 tick（防螺旋死亡），
    // 跑满仍超限 → 丢弃剩余累积时间（本帧已尽力，落后不欠账）。
    int steps = 0;
    while (accumulator_ >= 0.04 && steps < kMaxStepsPerFrame_) { // 25TPS, dt=0.04
        applyCommands(fromBusiness);
        snapshotPlayerBody(wallDt);

        // simTime_ 是跨线程共享（main 线程读取），必须走原子读改写。
        simTime_.store(simTime_.load(std::memory_order_relaxed) + 0.04,
                       std::memory_order_relaxed);
        accumulator_ -= 0.04;
        tickCount_.fetch_add(1, std::memory_order_relaxed);
        ++steps;
        maybeLog();
    }

    // 超限丢时间：若跑满 kMaxStepsPerFrame_ 仍欠账（本帧严重落后），
    // 丢弃剩余累积——下一帧从零开始，不把落后带进下一帧。
    if (accumulator_ >= 0.04) {
        accumulator_ = 0.0;
    }
}

void PhysicsWorld::applyCommands(engine::core::SPSCQueue<engine::shared::PhysicsCommand>& fromBusiness) {
    // 物理消费速率 = 25×scale TPS；业务生产速率固定 20/s 墙钟——
    // 慢放时物理消化变慢、指令堆积（processed < sent），加速时追赶。
    engine::shared::PhysicsCommand cmd;
    while (fromBusiness.pop(cmd)) {
        switch (cmd.type) {
        case engine::shared::PhysicsCommand::Type::MoveBlock:
            // [已废弃] 演示方块位移。物理侧不再维护"单个方块坐标"这种状态；
            // 指令仍保留在协议里（追加式演进，不删枚举值），物理侧直接忽略。
            break;
        case engine::shared::PhysicsCommand::Type::SyncBlockSet:
            onSyncBlockSet(cmd);
            break;
        case engine::shared::PhysicsCommand::Type::PlaceBlock:
            onPlaceBlock(cmd);
            break;
        case engine::shared::PhysicsCommand::Type::RemoveBlock:
            onRemoveBlock(cmd);
            break;
        case engine::shared::PhysicsCommand::Type::PackContraption:
            onPackContraption(cmd);
            break;
        case engine::shared::PhysicsCommand::Type::UnpackContraption:
            onUnpackContraption(cmd);
            break;
        }
        processedCommandCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PhysicsWorld::onSyncBlockSet(const engine::shared::PhysicsCommand& cmd) {
    // 全量同步：重建静态碰撞体集合（初始世界 / 世界重建时用）。
    // 注意：碰撞体集合目前只是"静态世界的影子"，没有碰撞消费方（求解器已删）。
    // Jolt 接入后这里改成"逐 chunk 建 StaticCompoundShape"，且要按脏 chunk 增量重建。
    state_.colliders().clear();
    for (const auto& b : cmd.blocks) state_.colliders()[b.pos] = b.type;
}

void PhysicsWorld::onPlaceBlock(const engine::shared::PhysicsCommand& cmd) {
    // 放置：业务裁决已通过。目标域（cmd.contraptionId）：
    // -1 = 主世界（建静态碰撞体）；≥0 = 刚体（飞船改造：方块加入刚体，
    //      增量重算碰撞箱——碰撞箱实时扩张贴合新方块组）。
    const engine::shared::BlockPos p{cmd.x, cmd.y, cmd.z};
    if (cmd.contraptionId < 0) {
        state_.colliders()[p] = static_cast<engine::shared::BlockType>(cmd.blockType);
        return;
    }
    const auto it = std::find_if(
        state_.bodies().begin(), state_.bodies().end(),
        [&](const RigidBody& b) { return b.id == cmd.contraptionId; });
    if (it != state_.bodies().end()) {
        it->blocks.push_back(engine::shared::Block{
            static_cast<engine::shared::BlockType>(cmd.blockType), p});
        it->blockCount = static_cast<int>(it->blocks.size());
        // 碰撞箱不再重算：Jolt 接入时 AddShape + AdjustCenterOfMass + NotifyShapeChanged。
    }
}

void PhysicsWorld::onRemoveBlock(const engine::shared::PhysicsCommand& cmd) {
    // 破坏：业务裁决已通过。目标域同上。
    const engine::shared::BlockPos p{cmd.x, cmd.y, cmd.z};
    if (cmd.contraptionId < 0) {
        state_.colliders().erase(p);
        return;
    }
    const auto it = std::find_if(
        state_.bodies().begin(), state_.bodies().end(),
        [&](const RigidBody& b) { return b.id == cmd.contraptionId; });
    if (it != state_.bodies().end()) {
        for (auto bIt = it->blocks.begin(); bIt != it->blocks.end(); ++bIt) {
            if (bIt->pos == p) { it->blocks.erase(bIt); break; }
        }
        it->blockCount = static_cast<int>(it->blocks.size());
        // 碰撞箱不再重算：Jolt 接入时 RemoveShape + AdjustCenterOfMass + NotifyShapeChanged。
    }
}

void PhysicsWorld::onPackContraption(const engine::shared::PhysicsCommand& cmd) {
    // 打包：新建刚体，应用业务裁决的初始速度（起飞速度）。
    // 物理形态转移（架构 2.7）：方块从静态碰撞体集合转移进刚体。
    RigidBody body;
    body.id = state_.allocateBodyId();
    body.blockCount = cmd.blockCount;
    body.vx = cmd.ivx;
    body.vy = cmd.ivy;
    body.vz = cmd.ivz;
    // [临时验证] 固定 45° 绕 Y 轴：只用来确认"旋转数据通路"通到渲染端。
    // 四元数 = (0, sin22.5°, 0, cos22.5°)。Jolt 接入后删掉这一行。
    body.qx = 0.0f; body.qy = 0.38268343f; body.qz = 0.0f; body.qw = 0.92387953f;
    body.blocks = cmd.blocks; // 局部坐标 = 世界坐标（刚体建在原点 0）
    for (const auto& b : cmd.blocks) state_.colliders().erase(b.pos);
    // 碰撞箱不再手算：Jolt 接入时由 MutableCompoundShape 的 AddShape 生成。
    state_.bodies().push_back(body);
    bodyCount_.store(static_cast<long long>(state_.bodies().size()),
                     std::memory_order_relaxed);
    // 回执：打包完成（回执必须逐条送达，不参与快照合并）。
    // 带局部坐标方块列表 + 刚体位置：业务不必自留副本，渲染端直接拿它建网格。
    engine::shared::PhysicsResult r;
    r.type = engine::shared::PhysicsResult::Type::ContraptionPacked;
    r.contraptionId = body.id;
    r.blockCount = body.blockCount;
    r.rx = body.x;
    r.ry = body.y;
    r.rz = body.z;
    r.blocks = body.blocks;
    pendingReceipts_.push_back(r);
}

void PhysicsWorld::onUnpackContraption(const engine::shared::PhysicsCommand& cmd) {
    // 解包：拆除刚体，方块按"基准+局部坐标"回写静态碰撞体集合。
    // 回执携带世界坐标方块列表：业务据此重建 blocks_（解包闭环，架构 2.7）。
    std::vector<engine::shared::Block> worldBlocks;
    const auto it = std::find_if(
        state_.bodies().begin(), state_.bodies().end(),
        [&](const RigidBody& b) { return b.id == cmd.contraptionId; });
    if (it != state_.bodies().end()) {
        worldBlocks.reserve(it->blocks.size());
        for (const auto& b : it->blocks) {
            const engine::shared::BlockPos world{
                static_cast<int>(std::llround(it->x + b.pos.x)),
                static_cast<int>(std::llround(it->y + b.pos.y)),
                static_cast<int>(std::llround(it->z + b.pos.z))};
            state_.colliders()[world] = b.type;
            worldBlocks.push_back(engine::shared::Block{b.type, world});
        }
        state_.bodies().erase(it);
    }
    bodyCount_.store(static_cast<long long>(state_.bodies().size()),
                     std::memory_order_relaxed);
    engine::shared::PhysicsResult r;
    r.type = engine::shared::PhysicsResult::Type::ContraptionUnpacked;
    r.contraptionId = cmd.contraptionId;
    r.blocks = std::move(worldBlocks);
    pendingReceipts_.push_back(r);
}

void PhysicsWorld::snapshotPlayerBody(double wallDt) {
    // ---- 刚体积分：已删除 ----
    for (auto& b : state_.bodies()) {
        if (b.motion != MotionType::Kinematic) continue;
        // 插值快照：先记下本 tick 起点的位置（= 上一 tick 的终点）。
        const bool   isPlayerBody = (b.id == state_.playerBodyId() && playerState_ != nullptr);
        const double tickStartX = b.x;
        const double tickStartY = b.y;
        const double tickStartZ = b.z;
        // 玩家位置回传（相机跟随 + 实体位姿）：只写共享槽，不占业务结果队列。
        // 一次成组写入 prev（本 tick 起点）/ curr（本 tick 终点）/ tick 起点时刻，
        // 整组用 seqlock 包住：读侧要么拿到同一 tick 的一对值，要么重试。
        // tickStartNs 取此刻：写完之后 alpha 从 0 涨到 1，正好滑到下一 tick 覆盖前，
        // 即标准的"画面比物理慢一个 tick"。
        if (isPlayerBody) {
            auto& st = *playerState_;
            const long long nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            // tickDurationNs：用上次写入到这次的墙钟差，自适应 time_scale 和调度抖动。
            // 物理是累加器驱动，墙钟间隔 = 0.04 / scale 秒；scale≠1 时写死 40ms 会让
            // 渲染端 alpha 的分母错误（scale=2 滞后半帧、scale=0.5 卡终点再跳）。
            // 首次写入（lastPlayerStateWriteNs_ == 0）用当前帧墙钟兜底。
            long long durNs = static_cast<long long>(wallDt * 1e9);
            if (lastPlayerStateWriteNs_ > 0 && nowNs > lastPlayerStateWriteNs_) {
                durNs = nowNs - lastPlayerStateWriteNs_;
            }

            st.seq.fetch_add(1, std::memory_order_acq_rel); // 奇数 = 写入中
            st.prevX.store(tickStartX, std::memory_order_relaxed);
            st.prevY.store(tickStartY, std::memory_order_relaxed);
            st.prevZ.store(tickStartZ, std::memory_order_relaxed);
            st.x.store(b.x, std::memory_order_relaxed);
            st.y.store(b.y, std::memory_order_relaxed);
            st.z.store(b.z, std::memory_order_relaxed);
            st.tickStartNs.store(nowNs, std::memory_order_relaxed);
            st.tickDurationNs.store(durNs, std::memory_order_relaxed);
            st.seq.fetch_add(1, std::memory_order_release); // 偶数 = 稳定
            st.valid.store(true, std::memory_order_relaxed);
            lastPlayerStateWriteNs_ = nowNs;
        }
    }
}

void PhysicsWorld::pushResultsToBusiness(
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
    // 每个刚体各发一条（生物进容器后不能再只发第一个刚体）。
    if (!state_.bodies().empty()) {
        const double st = simTime_.load(std::memory_order_relaxed);
        if (st != lastPosSentSimT_) { // 模拟时刻已推进 → 每个刚体各发一条新位置快照
            lastPosSentSimT_ = st;
            for (const RigidBody& b : state_.bodies()) {
                engine::shared::PhysicsResult pos;
                pos.type = engine::shared::PhysicsResult::Type::RigidBodyPos;
                pos.contraptionId = b.id;
                pos.rx = b.x;
                pos.ry = b.y;
                pos.rz = b.z;
                pos.qx = b.qx; pos.qy = b.qy; pos.qz = b.qz; pos.qw = b.qw;
                pos.simTime = st;
                q.push(pos); // 满则丢：业务消化不过来时丢弃旧快照可接受
            }
        }
    }

    // ③ 静态碰撞体数量快照（合并语义：业务只需最新值，用于两侧一致性核对）。
    {
        engine::shared::PhysicsResult set;
        set.type = engine::shared::PhysicsResult::Type::BlockSetSize;
        set.blockSetSize = static_cast<long long>(state_.colliders().size());
        set.simTime = simTime_.load(std::memory_order_relaxed);
        q.push(set); // 满则丢：快照合并语义，旧值丢弃可接受
    }

    // ④ 演示方块位置快照：只有本帧处理过新指令才发，避免每帧重复推快照塞满队列。
    const long long processed = processedCommandCount_.load(std::memory_order_relaxed);
    if (processed == lastSentProcessed_) return;
    lastSentProcessed_ = processed;

    engine::shared::PhysicsResult res;
    res.type = engine::shared::PhysicsResult::Type::BlockMoved;
    res.x = blockX_.load(std::memory_order_relaxed);
    res.y = blockY_.load(std::memory_order_relaxed);
    res.z = blockZ_.load(std::memory_order_relaxed);
    res.processedCommands = processed;   // 协议字段（shared/protocol，属附2 不改名）
    res.simTime = simTime_.load(std::memory_order_relaxed);
    q.push(res); // 满则丢：极端情况下业务消化不过来，丢弃快照可接受
}

double PhysicsWorld::simTime() const {
    return simTime_.load(std::memory_order_relaxed);
}
long long PhysicsWorld::tickCount() const {
    return tickCount_.load(std::memory_order_relaxed);
}
long long PhysicsWorld::processedCommandCount() const {
    return processedCommandCount_.load(std::memory_order_relaxed);
}
long long PhysicsWorld::bodyCount() const {
    return bodyCount_.load(std::memory_order_relaxed);
}
long long PhysicsWorld::colliderCount() const {
    // main 在物理线程停止后读取，碰撞体集合此时无并发写。
    return static_cast<long long>(state_.colliders().size());
}
long long PhysicsWorld::collisionCount() const {
    return collisionCount_.load(std::memory_order_relaxed);
}

// moveKinematicAxis：已删除（原 65 行，分轴移动 + 分轴回退 + onGround）。
// 由 Jolt CharacterVirtual 的 ExtendedUpdate 取代（斜坡 / 台阶 / 贴地吸附内置）。

// recomputeAABB：已删除（原 29 行，最小包裹 AABB + 三个原子投影）。
// 碰撞箱由 Jolt 的 compound shape 自带（GetLocalBounds），不需要手工重算。
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
           << " 处理指令=" << processedCommandCount()
           << " 刚体=" << bodyCount()
           << " 碰撞体=" << state_.colliders().size()
           << " 碰撞=" << collisionCount()
           << " 方块=(" << blockX() << "," << blockY() << "," << blockZ() << ")"
           << "  生效系数=" << tm_.effectiveScale(engine::core::Domain::Physics);
        // 各物理体位置（物理线程内遍历刚体表安全）：动态体与生物分开标名
        for (const auto& b : state_.bodies()) {
            if (b.motion == MotionType::Dynamic) {
                os << " 刚体" << b.id << "=(" << b.x << "," << b.y << "," << b.z << ")";
            } else if (b.motion == MotionType::Kinematic) {
                os << " 生物" << b.id << "=(" << b.x << "," << b.y << "," << b.z
                   << ") 在地面=" << (b.onGround ? 1 : 0);
            }
        }
        ENGINE_LOG_INFO("Physics", os.str());
    }
}

} // namespace engine::physics

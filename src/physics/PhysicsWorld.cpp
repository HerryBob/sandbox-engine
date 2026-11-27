#include "physics/PhysicsWorld.hpp"

#include "shared/block/StructureFrame.hpp"
#include "physics/JoltKernel.hpp"
#include "core/Log.hpp"
#include "shared/chunk/ChunkPos.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <unordered_set>

namespace engine::physics {

// ---- Jolt 后端（PhysicsWorld.hpp 里只前置声明，JPH:: 类型不出这条 TU）----
namespace {
// v1 层划分：静态地形 / 可动体。加玩家角色、触发器时在这里扩。
constexpr JPH::ObjectLayer kObjLayerNonMoving    = 0;
constexpr JPH::ObjectLayer kObjLayerMoving       = 1;
constexpr JPH::uint        kNumObjectLayers      = 2;
constexpr JPH::uint        kNumBroadPhaseLayers  = 2;

// 方块集合签名的单块散列（坐标 + 类型；与顺序无关地累加）。
inline std::uint64_t blockHash(const engine::shared::Block& b) {
    std::uint64_t h = static_cast<std::uint64_t>(b.pos.x) * 73856093ull;
    h ^= static_cast<std::uint64_t>(b.pos.y) * 19349663ull;
    h ^= static_cast<std::uint64_t>(b.pos.z) * 83492791ull;
    h = h * 31ull + static_cast<std::uint64_t>(b.type);
    return h;
}

// ---- 层接口：手写三个小类（与 Jolt HelloWorld 同款）----
// 为什么不用 `*Table` 便捷类：实测它们的语义和"查询对象层 × 刚体 broadphase 层"
// 的组合对不上，导致角色的碰撞查询与射线全部打空（无过滤器时能命中，带过滤器就 0 命中）。
// 手写虽然多 20 行，但每一处判断都是显式的，不会猜错方向。
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return kNumBroadPhaseLayers; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        return JPH::BroadPhaseLayer(inLayer == kObjLayerNonMoving ? 0 : 1);
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        return inLayer == JPH::BroadPhaseLayer(0) ? "NON_MOVING" : "MOVING";
    }
#endif
};

class ObjectVsBroadPhaseFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    // 宽进：真正的"能不能碰"由 ObjectLayerPairFilter 判定。
    bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return true; }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inA, JPH::ObjectLayer inB) const override {
        // 两块静态地形之间不用检测（永远不移动）；其余都检测。
        if (inA == kObjLayerNonMoving && inB == kObjLayerNonMoving) return false;
        return true;
    }
};
} // namespace

struct JoltBackend {
    // 声明顺序 = 构造顺序：层接口 → 过滤器 → 分配器/作业系统 → PhysicsSystem
    BPLayerInterfaceImpl          bpLayers;
    ObjectLayerPairFilterImpl     objPairFilter;
    ObjectVsBroadPhaseFilterImpl  objVsBpFilter;
    JPH::TempAllocatorImpl        tempAllocator;
    JPH::JobSystemSingleThreaded  jobSystem;
    JPH::PhysicsSystem            system;
    // 每 chunk 一个静态刚体（StaticCompoundShape：一个 chunk 的方块各是一个盒子）。
    // 放置/破坏时只重建受影响的那个 chunk。
    std::unordered_map<engine::shared::ChunkPos, JPH::BodyID> chunkBodies;
    // 每 chunk 的方块集合签名：全量同步时只有签名变化的 chunk 才重建 Jolt 形状。
    // 没有它，每次 SyncBlockSet（流式每加载一批就发一次）都要重建全部 chunk 形状
    // —— 实测 82,944 方块 = 250ms/次，直接把物理线程卡住。
    std::unordered_map<engine::shared::ChunkPos, std::uint64_t> chunkSignatures;
    // 玩家角色（CharacterVirtual）：不是刚体，靠碰撞查询移动。
    // 声明在 system 之后 → 析构时先于 system 销毁（顺序必须正确）。
    JPH::Ref<JPH::CharacterVirtual> player;
    // 结构 id → 动态刚体（打包的 contraption）。解包时移除。
    std::unordered_map<int, JPH::BodyID> contraptionBodies;

    JoltBackend()
        : tempAllocator(16 * 1024 * 1024),
          jobSystem(4096) {
        // maxBodies / bodyMutexes(0=自动) / maxBodyPairs / maxContactConstraints
        system.Init(1024, 0, 8192, 2048, bpLayers, objVsBpFilter, objPairFilter);
        system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    }

    void removeChunkBody(const engine::shared::ChunkPos& cp) {
        const auto it = chunkBodies.find(cp);
        if (it == chunkBodies.end()) return;
        system.GetBodyInterface().RemoveBody(it->second);
        chunkBodies.erase(it);
    }
    void removeAllChunkBodies() {
        for (const auto& [cp, id] : chunkBodies) system.GetBodyInterface().RemoveBody(id);
        chunkBodies.clear();
        chunkSignatures.clear();   // 签名与 body 同生共死；不清理会让下次同步误判"未变"而跳过重建
    }
    void removeContraptionBody(int structureId) {
        const auto it = contraptionBodies.find(structureId);
        if (it == contraptionBodies.end()) return;
        system.GetBodyInterface().RemoveBody(it->second);
        contraptionBodies.erase(it);
    }

    // Jolt 要求 PhysicsSystem 析构前所有刚体已移除，否则 BodyManager 的析构断言触发 __debugbreak。
    // player 不是刚体，但内部持 Shape 引用且依赖 system 的查询接口，先释放。
    ~JoltBackend() {
        player = nullptr;
        removeAllChunkBodies();
        for (const auto& entry : contraptionBodies)
            system.GetBodyInterface().RemoveBody(entry.second);
        contraptionBodies.clear();
    }
};

PhysicsWorld::~PhysicsWorld() = default;

PhysicsWorld::PhysicsWorld(engine::core::TimeManager& tm)
    : tm_(tm),
      startWall_(std::chrono::steady_clock::now()),
      lastLog_(std::chrono::steady_clock::now()) {
    // 内核是进程级单例（JoltKernel 幂等）：自检可能已经初始化过。
    initJoltKernel();
    jolt_ = std::make_unique<JoltBackend>();
}

int PhysicsWorld::addPlayerBody(double x, double y, double z,
                                double halfExtentX, double halfExtentY, double halfExtentZ,
                                double jumpSpeed) {
    // 生物体：运动学，位置锚在脚底中心。
    // 形状与跳跃初速来自实体类型定义（经装配层传入），本文件不写死某个生物的尺寸。
    // 尺寸存进 halfExtent（作为 BoxShape 的半尺寸）；
    // centerOffsetY = 半高，让"基准 y = 脚底"成立。
    PhysicsBody b;
    b.bodyId = state_.allocateBodyId();
    b.structureId = -1;                       // 玩家不属于任何结构
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
    state_.setPlayerBodyId(b.bodyId);

    // Jolt 侧：长方体碰撞箱，尺寸直接来自实体定义（宽×高×深 = 0.8×1.8×0.8）。
    // 盒子带直角，在方块拼缝上滑动会被 ghost contact 绊住 —— 靠 mEnhancedInternalEdgeRemoval 缓解。
    JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3(
        static_cast<float>(halfExtentX),
        static_cast<float>(halfExtentY),
        static_cast<float>(halfExtentZ)));
    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape = shape;
    settings->mMaxSlopeAngle = JPH::DegreesToRadians(45.0f);
    settings->mMaxStrength = 100.0f;
    settings->mShapeOffset = JPH::Vec3::sZero();
    settings->mEnhancedInternalEdgeRemoval = true;
    jolt_->player = new JPH::CharacterVirtual(
        settings, JPH::RVec3(x, y + halfExtentY, z), JPH::Quat::sIdentity(), &jolt_->system);
    return b.bodyId;
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
        drainPendingChunkRebuilds();   // 每 tick 只重建 1 个待处理 chunk（预算式）
        stepJolt();            // 推进 Dynamic 刚体（无动态体时内部直接跳过）
        syncContraptionPoses();// Jolt 位姿 → 数据体（渲染与回执都读数据体）
        publishPoseFrame();    // 每 tick 发布一帧到跨线程位姿表（玩家 + 刚体）
        stepPlayer(0.04);      // 推进玩家角色（CharacterVirtual）
        snapshotPlayerBody(wallDt);   // 推一条快照进模拟时间轴环（历史在环里，不需 prev）

        // simTime_ 是跨线程共享（main 线程读取），必须走原子读改写。
        simTime_.store(simTime_.load(std::memory_order_relaxed) + 0.04,
                       std::memory_order_relaxed);
        accumulator_ -= 0.04;
        tickCount_.fetch_add(1, std::memory_order_relaxed);
        ++steps;
        maybeLog();
    }

    // 欠账处理：只在"真卡顿"（>320ms 落后）时才丢弃，且丢到只剩一步；
    // 一般落后（<320ms）保留欠账，接下来几帧逐帧追上 —— 不丢时间，视觉上也更连续。
    if (accumulator_ > kMaxAccumulator_) {
        accumulator_ = 0.04;
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
        case engine::shared::PhysicsCommand::Type::SyncChunks:
            onSyncChunks(cmd);
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

// 增量同步：只动"这次真变了的 chunk"。
// 数据层按 chunk 区域先清后填（4096 次查找/块，很便宜）；
// Jolt 侧的重建**入队**，由 drainPendingChunkRebuilds 每 tick 只做 1 个。
void PhysicsWorld::onSyncChunks(const engine::shared::PhysicsCommand& cmd) {
    for (const auto& cp : cmd.changedChunks) {
        const int bx = cp.x << 4, by = cp.y << 4, bz = cp.z << 4;
        for (int x = bx; x < bx + 16; ++x)
            for (int y = by; y < by + 16; ++y)
                for (int z = bz; z < bz + 16; ++z)
                    state_.colliders().erase(engine::shared::BlockPos{x, y, z});
    }
    for (const auto& b : cmd.blocks) state_.colliders()[b.pos] = b.type;

    // 入队（去重：同一 chunk 本 tick 已经排队就不重复）
    for (const auto& cp : cmd.changedChunks) {
        bool exists = false;
        for (const auto& q : pendingChunkRebuilds_) {
            if (q == cp) { exists = true; break; }
        }
        if (!exists) pendingChunkRebuilds_.push_back(cp);
    }
}

// 预算：每 tick 按**时间**重建 pending chunk（单块约 2.4ms，上限 3ms ≈ 1~2 块）。
// 用时间而不是固定条数：块大小可能差很多（空 chunk 极快，满 chunk 2.4ms）。
void PhysicsWorld::drainPendingChunkRebuilds() {
    const auto t0 = std::chrono::steady_clock::now();
    while (!pendingChunkRebuilds_.empty()) {
        const auto cp = pendingChunkRebuilds_.front();
        pendingChunkRebuilds_.erase(pendingChunkRebuilds_.begin());
        rebuildChunkStatic(cp.x, cp.y, cp.z);
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        if (ms >= 3.0) break;   // 超过预算，剩下的下个 tick 继续
    }
}

void PhysicsWorld::onSyncBlockSet(const engine::shared::PhysicsCommand& cmd) {
    // 全量同步：业务侧一次给全部方块（初始世界 / 世界重建 / 流式加载后各发一次）。
    // 数据层照旧整体重建；**Jolt 侧只重建"方块集合真变了的 chunk"** ——
    // 否则每次流式加载都要重建 81 个 chunk 的复合形状（约 250ms），物理线程会被卡死。
    state_.colliders().clear();
    std::unordered_map<engine::shared::ChunkPos, std::uint64_t> incoming;
    incoming.reserve(128);
    for (const auto& b : cmd.blocks) {
        state_.colliders()[b.pos] = b.type;
        const auto cp = engine::shared::toChunkOf(b.pos);
        incoming[cp] += blockHash(b);
    }

    const auto t0 = std::chrono::steady_clock::now();
    int rebuilt = 0;
    for (const auto& [cp, sig] : incoming) {
        const auto it = jolt_->chunkSignatures.find(cp);
        if (it == jolt_->chunkSignatures.end() || it->second != sig) {
            rebuildChunkStatic(cp.x, cp.y, cp.z);   // 内部更新签名
            ++rebuilt;
        }
    }
    // 这次同步里消失的 chunk → 删掉它的静态刚体
    int removed = 0;
    for (auto it = jolt_->chunkSignatures.begin(); it != jolt_->chunkSignatures.end();) {
        if (incoming.find(it->first) == incoming.end()) {
            jolt_->removeChunkBody(it->first);
            it = jolt_->chunkSignatures.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    // 大量刚体在首次 Update 之前加入 → 按 Jolt 文档建议优化一次 broadphase
    jolt_->system.OptimizeBroadPhase();
    {
        std::ostringstream os;
        os << "静态碰撞体同步：方块=" << state_.colliders().size()
           << " chunk=" << incoming.size()
           << " 重建=" << rebuilt << " 移除=" << removed
           << " 耗时=" << ms << "ms";
        ENGINE_LOG_INFO("Physics", os.str());

    }
}

// 重建一个 chunk 的静态形状：该 chunk 内所有方块各成一个盒子，合成一个复合形状。
// v1 取舍：整 chunk 重建（1024 个盒子），放置/破坏是低频操作，先要正确；
// 将来要优化就换 MutableCompoundShape 的增删接口。
void PhysicsWorld::rebuildChunkStatic(int chunkX, int chunkY, int chunkZ) {
    const engine::shared::ChunkPos cp{chunkX, chunkY, chunkZ};
    jolt_->removeChunkBody(cp);

    const int bx = chunkX << 4, by = chunkY << 4, bz = chunkZ << 4;
    JPH::StaticCompoundShapeSettings compound;
    // 所有方块都是同一个 0.5³ 盒子 → 共用一份 BoxShapeSettings（省掉每块一次分配）。
    // 盒子设置必须堆分配并被 Ref 托管：AddShape 会加引用计数，栈对象会在计数归零前析构
    //（Jolt Reference.h 会断言）。
    JPH::RefConst<JPH::ShapeSettings> box =
        new JPH::BoxShapeSettings(JPH::Vec3(0.5f, 0.5f, 0.5f));
    int boxCount = 0;
    for (int x = bx; x < bx + 16; ++x) {
        for (int y = by; y < by + 16; ++y) {
            for (int z = bz; z < bz + 16; ++z) {
                const auto it = state_.colliders().find(engine::shared::BlockPos{x, y, z});
                if (it == state_.colliders().end()) continue;
                compound.AddShape(
                    JPH::Vec3(static_cast<float>(x) + 0.5f,
                              static_cast<float>(y) + 0.5f,
                              static_cast<float>(z) + 0.5f),
                    JPH::Quat::sIdentity(), box.GetPtr());
                ++boxCount;
            }
        }
    }
    if (boxCount == 0) return;   // 空 chunk 不建刚体（与"空 chunk 不占内存"一致）

    JPH::ShapeSettings::ShapeResult result = compound.Create();
    if (result.HasError()) {
        ENGINE_LOG_ERROR("Physics", std::string("chunk 静态形状创建失败：") + result.GetError().c_str());
        return;
    }
    JPH::BodyCreationSettings settings(
        result.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, kObjLayerNonMoving);
    jolt_->chunkBodies[cp] =
        jolt_->system.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
    // 记录该 chunk 的方块集合签名（放置/破坏与全量同步共用这一份，保证比较基准一致）
    jolt_->chunkSignatures[cp] = chunkSignature(chunkX, chunkY, chunkZ);
}

// 一个 chunk 内方块集合的签名：与顺序无关（逐块散列后累加）。
// 用途：全量同步时判断"这个 chunk 的方块到底变了没有"，避免无谓重建。
std::uint64_t PhysicsWorld::chunkSignature(int chunkX, int chunkY, int chunkZ) const {
    std::uint64_t sum = 0;
    const int bx = chunkX << 4, by = chunkY << 4, bz = chunkZ << 4;
    for (int x = bx; x < bx + 16; ++x) {
        for (int y = by; y < by + 16; ++y) {
            for (int z = bz; z < bz + 16; ++z) {
                const auto it = state_.colliders().find(engine::shared::BlockPos{x, y, z});
                if (it == state_.colliders().end()) continue;
                sum += blockHash(engine::shared::Block{it->second, {x, y, z}});
            }
        }
    }
    return sum;
}

void PhysicsWorld::stepJolt() {
    if (!jolt_) return;
    // 没有动态刚体时跳过推进：静态世界不需要模拟。
    // 玩家是 CharacterVirtual（不是刚体，自己走碰撞查询），所以这里只负责 Dynamic 刚体。
    //
    // ⚠ 这里**不能**用 GetNumActiveBodies 做判断（曾经就是这么写的）：
    //   "active" = **醒着** 的刚体。Jolt 会在刚体静止后让它睡眠 → 活跃数变 0
    //   → Update 永远不再被调用 → 重力再也不积分 → 刚体被冻在原地
    //   （表现：刚体只在玩家碰到它时动一下，因为接触把它唤醒、那一 tick 才 Update）。
    //   现在的判据是"有没有动态刚体"：刚体睡着也必须继续 Update，
    //   由 Jolt 自己决定要不要算（全睡眠时开销很小）。
    if (jolt_->contraptionBodies.empty()) return;
    // 固定步长 25Hz + 2 次碰撞子步（与《未来方向-物理内核替换为Jolt》§六 一致）
    jolt_->system.Update(0.04f, 2, &jolt_->tempAllocator, &jolt_->jobSystem);
}

// 玩家角色推进：水平速度来自输入意图，垂直由跳跃/重力决定；位置回写数据体。
// 数据层位置语义 = 脚底中心，CharacterVirtual 的位置 = 盒子中心，这里做一次换算。
void PhysicsWorld::stepPlayer(double dt) {
    if (!jolt_ || !jolt_->player) return;
    JPH::CharacterVirtual* ch = jolt_->player.GetPtr();

    JPH::Vec3 vel = ch->GetLinearVelocity();
    const bool supported = ch->IsSupported();
    if (playerControl_) {
        // 水平：渲染线程给的是世界坐标目标速度（格/秒）
        vel.SetX(static_cast<float>(playerControl_->vx.load(std::memory_order_relaxed)));
        vel.SetZ(static_cast<float>(playerControl_->vz.load(std::memory_order_relaxed)));
        // 跳跃：只在支撑面（地面/缓坡）上生效，按下边沿已被渲染线程置位
        const bool wantJump =
            playerControl_->jumpRequested.exchange(false, std::memory_order_relaxed);
        if (wantJump && supported) vel.SetY(static_cast<float>(jumpSpeed_));
    }
    // 重力必须自己积分：Jolt 的 CharacterVirtual::Update 只在"站在别的物体上"时用重力
    // 算下压量，明确说明"apply gravity to velocity 是你自己的责任"（CharacterVirtual.h:386）。
    if (!supported) {
        float vy = vel.GetY() + jolt_->system.GetGravity().GetY() * static_cast<float>(dt);
        if (vy < static_cast<float>(kTerminalFallSpeed_)) vy = static_cast<float>(kTerminalFallSpeed_);
        vel.SetY(vy);
    } else if (vel.GetY() < 0.0f) {
        vel.SetY(0.0f);   // 贴地：不累积向下速度
    }
    ch->SetLinearVelocity(vel);

    // 推进：Jolt 自己处理重力、贴地、台阶与坡度限制
    const JPH::ObjectLayer layer = kObjLayerMoving;
    ch->Update(static_cast<float>(dt), jolt_->system.GetGravity(),
               jolt_->system.GetDefaultBroadPhaseLayerFilter(layer),
               jolt_->system.GetDefaultLayerFilter(layer),
               JPH::BodyFilter(), JPH::ShapeFilter(), jolt_->tempAllocator);

    // 回写数据体（脚底中心）
    const JPH::RVec3 p = ch->GetPosition();
    if (PhysicsBody* b = state_.findBodyById(state_.playerBodyId())) {
        b->x = p.GetX();
        b->y = p.GetY() - b->halfExtentY;
        b->z = p.GetZ();
        b->onGround = ch->IsSupported();
    }
}

// 把 Jolt 的动态刚体位姿回写到数据体（位置 + 旋转）。
// 渲染端的刚体网格与 RigidBodyPos 回执都读数据体，所以这里必须紧随 stepJolt 之后。
void PhysicsWorld::syncContraptionPoses() {
    if (!jolt_) return;
    JPH::BodyInterface& bi = jolt_->system.GetBodyInterface();
    for (auto& b : state_.bodies()) {
        if (b.structureId < 0) continue;                       // 玩家不走这条路
        const auto it = jolt_->contraptionBodies.find(b.structureId);
        if (it == jolt_->contraptionBodies.end()) continue;
        const JPH::RVec3 p = bi.GetPosition(it->second);
        const JPH::Quat  q = bi.GetRotation(it->second);
        b.x = p.GetX(); b.y = p.GetY(); b.z = p.GetZ();
        b.qx = q.GetX(); b.qy = q.GetY(); b.qz = q.GetZ(); b.qw = q.GetW();
    }
}

// 每 tick 发布一帧位姿到跨线程表（玩家 slot 0 + 所有刚体）。
// 只是一次整表发布：物理线程填完 → 一次 release store 翻转。
void PhysicsWorld::publishPoseFrame() {
    if (poseHistory_ == nullptr) return;
    engine::shared::PoseFrame& f = poseHistory_->beginFrame(tickCount_ + 1);
    // 玩家
    if (const PhysicsBody* pb = state_.findBodyById(state_.playerBodyId())) {
        engine::shared::PoseEntry& e = f.e[poseHistory_->playerSlot()];
        e.px = static_cast<float>(pb->x);
        e.py = static_cast<float>(pb->y);
        e.pz = static_cast<float>(pb->z);
        e.qx = static_cast<float>(pb->qx);
        e.qy = static_cast<float>(pb->qy);
        e.qz = static_cast<float>(pb->qz);
        e.qw = static_cast<float>(pb->qw);
    }
    // 刚体
    for (const PhysicsBody& b : state_.bodies()) {
        if (b.structureId < 0) continue;
        const int slot = poseHistory_->slotOfStructure(b.structureId);
        if (slot < 0) continue;
        engine::shared::PoseEntry& e = f.e[slot];
        e.px = static_cast<float>(b.x);
        e.py = static_cast<float>(b.y);
        e.pz = static_cast<float>(b.z);
        e.qx = static_cast<float>(b.qx);
        e.qy = static_cast<float>(b.qy);
        e.qz = static_cast<float>(b.qz);
        e.qw = static_cast<float>(b.qw);
    }
    poseHistory_->commitFrame();
}

void PhysicsWorld::onPlaceBlock(const engine::shared::PhysicsCommand& cmd) {
    // 放置：业务裁决已通过。目标域（cmd.contraptionId）：
    // -1 = 主世界（建静态碰撞体）；≥0 = 刚体（飞船改造：方块加入刚体，
    //      增量重算碰撞箱——碰撞箱实时扩张贴合新方块组）。
    const engine::shared::BlockPos p{cmd.x, cmd.y, cmd.z};
    if (cmd.contraptionId < 0) {
        state_.colliders()[p] = static_cast<engine::shared::BlockType>(cmd.blockType);
        const auto cp = engine::shared::toChunkOf(p);
        rebuildChunkStatic(cp.x, cp.y, cp.z);   // Jolt 侧同步：只重建这个 chunk
        return;
    }
    PhysicsBody* it = state_.findBodyByStructure(cmd.contraptionId);
    if (it != nullptr) {
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
        const auto cp = engine::shared::toChunkOf(p);
        rebuildChunkStatic(cp.x, cp.y, cp.z);   // Jolt 侧同步：只重建这个 chunk
        return;
    }
    PhysicsBody* it = state_.findBodyByStructure(cmd.contraptionId);
    if (it != nullptr) {
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
    PhysicsBody body;
    body.bodyId = state_.allocateBodyId();
    // 1.0：一个结构一个刚体 → 结构 id 直接沿用刚体 id（协议里的 contraptionId 不变）。
    // 多刚体阶段结构 id 会有自己的分配器，届时改成读命令携带的结构 id。
    body.structureId = body.bodyId;
    body.blockCount = cmd.blockCount;
    body.vx = cmd.ivx;
    body.vy = cmd.ivy;
    body.vz = cmd.ivz;

    // 把方块从世界坐标重定到"刚体原点"的局部坐标（原点取方块包围盒中心，取整）。
    // 为什么必须重定：渲染端拿局部坐标建网格、再乘刚体位姿。若局部坐标仍是世界坐标，
    // 一旦刚体旋转就会绕世界原点转，而不是绕自己转。
    int minX = INT32_MAX, minY = INT32_MAX, minZ = INT32_MAX;
    int maxX = INT32_MIN, maxY = INT32_MIN, maxZ = INT32_MIN;
    for (const auto& b : cmd.blocks) {
        minX = std::min(minX, b.pos.x); maxX = std::max(maxX, b.pos.x);
        minY = std::min(minY, b.pos.y); maxY = std::max(maxY, b.pos.y);
        minZ = std::min(minZ, b.pos.z); maxZ = std::max(maxZ, b.pos.z);
    }
    // B141：原点取**几何中心**（可能半整数），不是一个格点。
    // 局部坐标仍是整数（origin = 整数基准），形状/网格按 localBlockCenter 摆放，
    // 三者由 shared/block/StructureFrame.hpp 统一给出（物理与渲染共用同一份数学）。
    const int originX = engine::shared::structureOriginAxis(minX, maxX);
    const int originY = engine::shared::structureOriginAxis(minY, maxY);
    const int originZ = engine::shared::structureOriginAxis(minZ, maxZ);
    const glm::dvec3 frameBias = engine::shared::structureBias(cmd.blocks);
    body.x = static_cast<double>(originX) + frameBias.x;
    body.y = static_cast<double>(originY) + frameBias.y;
    body.z = static_cast<double>(originZ) + frameBias.z;
    body.blocks.clear();
    body.blocks.reserve(cmd.blocks.size());
    for (const auto& b : cmd.blocks) {
        body.blocks.push_back(engine::shared::Block{
            b.type, engine::shared::BlockPos{b.pos.x - originX, b.pos.y - originY, b.pos.z - originZ}});
    }

    for (const auto& b : cmd.blocks) state_.colliders().erase(b.pos);
    // Jolt 侧同步：这些方块从静态世界移出 → 重建受影响的 chunk 形状
    {
        std::unordered_set<engine::shared::ChunkPos> touched;
        for (const auto& b : cmd.blocks) touched.insert(engine::shared::toChunkOf(b.pos));
        for (const auto& cp : touched) rebuildChunkStatic(cp.x, cp.y, cp.z);
    }

    // Jolt 侧：局部方块各一个盒子 → MutableCompoundShape（将来方块增删要改它）→ Dynamic 刚体
    {
        JPH::MutableCompoundShapeSettings compound;
        JPH::RefConst<JPH::ShapeSettings> box =
            new JPH::BoxShapeSettings(JPH::Vec3(0.5f, 0.5f, 0.5f));
        for (const auto& b : body.blocks) {
            // 局部方块中心（以几何中心为 0）—— 与渲染网格顶点用同一个函数
            const glm::vec3 c = engine::shared::localBlockCenter(b.pos, frameBias);
            compound.AddShape(JPH::Vec3(c.x, c.y, c.z), JPH::Quat::sIdentity(), box.GetPtr());
        }
        JPH::ShapeSettings::ShapeResult result = compound.Create();
        if (result.HasError()) {
            ENGINE_LOG_ERROR("Physics",
                std::string("刚体复合形状创建失败：") + result.GetError().c_str());
        } else {
            JPH::BodyCreationSettings settings(
                result.Get(), JPH::RVec3(body.x, body.y, body.z), JPH::Quat::sIdentity(),
                JPH::EMotionType::Dynamic, kObjLayerMoving);
            settings.mLinearVelocity =
                JPH::Vec3(static_cast<float>(cmd.ivx), static_cast<float>(cmd.ivy),
                          static_cast<float>(cmd.ivz));
            // 原型阶段**禁止刚体睡眠**：Jolt 会在静止约 0.5s 后让它睡（active=0），
            // 而"睡眠体"不会因为我们挖掉它脚下的方块、改地形而自动醒来
            // → 表现就是"看着像冻住了，只有玩家碰到才动一下"。
            // 刚体数量还很小（1~几个），一直模拟的开销可忽略；等多刚体/大量结构时
            // 再改成"按需唤醒"（在方块变更/邻近结构变动时显式 ActivateBody）。
            settings.mAllowSleeping = false;
            jolt_->contraptionBodies[body.structureId] =
                jolt_->system.GetBodyInterface().CreateAndAddBody(
                    settings, JPH::EActivation::Activate);
        }
    }

    state_.bodies().push_back(body);
    state_.bindStructure(body.structureId, body.bodyId);
    // 分配位姿槽（PoseHistory 启用时）：渲染端按 structureId 反查，O(1)
    if (poseHistory_ != nullptr && body.structureId >= 0) {
        poseHistory_->allocate(engine::shared::PoseKind::Contraption, body.structureId);
    }
    bodyCount_.store(static_cast<long long>(state_.bodies().size()),
                     std::memory_order_relaxed);
    // 回执：打包完成（回执必须逐条送达，不参与快照合并）。
    // 带局部坐标方块列表 + 刚体位置：业务不必自留副本，渲染端直接拿它建网格。
    engine::shared::PhysicsResult r;
    r.type = engine::shared::PhysicsResult::Type::ContraptionPacked;
    r.contraptionId = body.structureId;
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
    PhysicsBody* it = state_.findBodyByStructure(cmd.contraptionId);
    if (it != nullptr) {
        const int structureId = it->structureId;
        worldBlocks.reserve(it->blocks.size());
        // 局部方块中心 = p + 0.5 - bias（见 StructureFrame.hpp）；
        // 世界方块坐标 = 中心 - 0.5 = body.pos + p - bias
        const glm::dvec3 bias = engine::shared::structureBias(it->blocks);
        for (const auto& b : it->blocks) {
            const engine::shared::BlockPos world{
                static_cast<int>(std::llround(it->x + static_cast<double>(b.pos.x) - bias.x)),
                static_cast<int>(std::llround(it->y + static_cast<double>(b.pos.y) - bias.y)),
                static_cast<int>(std::llround(it->z + static_cast<double>(b.pos.z) - bias.z))};
            state_.colliders()[world] = b.type;
            worldBlocks.push_back(engine::shared::Block{b.type, world});
        }
        state_.unbindStructure(it->structureId);
        jolt_->removeContraptionBody(structureId);   // Jolt 侧：销毁动态刚体
        // 位姿表槽必须归还：否则每次"打包→解包"泄漏一个槽（2048 个之后新刚体
        // 分配不到槽 → 位姿不再发布 → 刚体不渲染）。释放后 slotOfStructure 返回 -1，
        // 渲染端下一帧就查不到它，网格也由 destroy 事件销毁。
        if (poseHistory_ != nullptr) {
            const int slot = poseHistory_->slotOfStructure(structureId);
            if (slot >= 0) poseHistory_->release(slot);
        }
        state_.bodies().erase(state_.bodies().begin() + (it - state_.bodies().data()));
    }
    bodyCount_.store(static_cast<long long>(state_.bodies().size()),
                     std::memory_order_relaxed);
    // Jolt 侧同步：方块回到静态世界 → 重建受影响的 chunk 形状
    {
        std::unordered_set<engine::shared::ChunkPos> touched;
        for (const auto& b : worldBlocks) touched.insert(engine::shared::toChunkOf(b.pos));
        for (const auto& cp : touched) rebuildChunkStatic(cp.x, cp.y, cp.z);
    }
    engine::shared::PhysicsResult r;
    r.type = engine::shared::PhysicsResult::Type::ContraptionUnpacked;
    r.contraptionId = cmd.contraptionId;
    r.blocks = std::move(worldBlocks);
    pendingReceipts_.push_back(r);
}

void PhysicsWorld::snapshotPlayerBody(double wallDt) {
    for (auto& b : state_.bodies()) {
        if (b.motion != MotionType::Kinematic) continue;
        const bool   isPlayerBody = (b.bodyId == state_.playerBodyId() && playerState_ != nullptr);
        // 玩家位置回传（相机跟随 + 实体位姿）：只写共享槽，不占业务结果队列。
        if (isPlayerBody) {
            auto& st = *playerState_;
            const long long nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            // tickDurationNs：**平滑后的实测间隔**（指数平均），不是标称值也不是原始间隔。
            // 两个极端都试过、都有问题：
            //   原始实测间隔（26~52ms 抖）→ 渲染速度忽快忽慢（逐帧位移 0.01~0.15 乱跳）
            //   标称 0.04/scale（恒定 40ms）→ 实际 tick 间隔 42~48ms 时 alpha 提前顶到 1
            //     并钳住（实测 min=0 的帧），下一 tick 再跳一次（max=2.4×avg）
            // 每 tick 的墙钟周期：**常量**（0.04/scale），不是实测间隔的 EMA。
            // 为什么必须常量：渲染端算 renderTick = (now - origin) / step。
            // 若 step 每 tick 都在变，运行 10 秒后哪怕 step 抖 0.2ms，
            // renderTick 也会跳 ~50ms —— 抖动幅度反而被放大（实测"幅度变大"）。
            // MC 的 tickLength 也是常量（50ms），同理。
            const double physScale = tm_.scale();
            const long long durNs = static_cast<long long>(
                0.04 / (physScale > 1e-6 ? physScale : 1.0) * 1e9);

            // 推一条快照（位置 + 整数 tick 号 + 每 tick 墙钟周期）。
            // 时间轴由渲染端用本地单调时钟自己算（MC 的 Timer 模型），
            // 不依赖"这条快照什么时候到达"。
            // MC 同款：只发布"整数 tick 号 + 每 tick 墙钟周期"，时间轴由渲染端自己算。
            const std::int64_t tickNo =
                tickCount_.load(std::memory_order_relaxed) + 1;
            st.push(b.x, b.y, b.z, tickNo, durNs);
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
            for (const PhysicsBody& b : state_.bodies()) {
                engine::shared::PhysicsResult pos;
                pos.type = engine::shared::PhysicsResult::Type::RigidBodyPos;
                pos.contraptionId = (b.structureId >= 0) ? b.structureId : b.bodyId;
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
           << "  生效系数=" << tm_.scale();
        // 各物理体位置（物理线程内遍历刚体表安全）：动态体与生物分开标名
        for (const auto& b : state_.bodies()) {
            if (b.motion == MotionType::Dynamic) {
                os << " 刚体" << b.bodyId << "=(" << b.x << "," << b.y << "," << b.z << ")";
            } else if (b.motion == MotionType::Kinematic) {
                os << " 生物" << b.bodyId << "=(" << b.x << "," << b.y << "," << b.z
                   << ") 在地面=" << (b.onGround ? 1 : 0);
            }
        }
        ENGINE_LOG_INFO("Physics", os.str());
    }
}

} // namespace engine::physics

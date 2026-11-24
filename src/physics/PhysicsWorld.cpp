#include "physics/PhysicsWorld.hpp"
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
    // 尺寸存进 halfExtent（Jolt 接入后作为 CapsuleShape 的参数）；
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

    // Jolt 侧：角色盒以"脚底 + 半高"为中心（数据层的位置语义是脚底中心）。
    JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(
        JPH::Vec3(static_cast<float>(halfExtentX),
                  static_cast<float>(halfExtentY),
                  static_cast<float>(halfExtentZ)));
    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape = shape;
    settings->mMaxSlopeAngle = JPH::DegreesToRadians(45.0f);
    settings->mMaxStrength = 100.0f;
    settings->mShapeOffset = JPH::Vec3::sZero();
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
        stepJolt();            // 推进 Dynamic 刚体（无动态体时内部直接跳过）
        stepPlayer(0.04);      // 推进玩家角色（CharacterVirtual）
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
    if (jolt_->system.GetNumActiveBodies(JPH::EBodyType::RigidBody) == 0) return;
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
        // 水平：输入线程给的是世界坐标目标速度（格/秒）
        vel.SetX(static_cast<float>(playerControl_->vx.load(std::memory_order_relaxed)));
        vel.SetZ(static_cast<float>(playerControl_->vz.load(std::memory_order_relaxed)));
        // 跳跃：只在支撑面（地面/缓坡）上生效，按下边沿已被输入线程置位
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
    // [临时验证] 固定 45° 绕 Y 轴：只用来确认"旋转数据通路"通到渲染端。
    // 四元数 = (0, sin22.5°, 0, cos22.5°)。Jolt 接入后删掉这一行。
    body.qx = 0.0f; body.qy = 0.38268343f; body.qz = 0.0f; body.qw = 0.92387953f;
    body.blocks = cmd.blocks; // 局部坐标 = 世界坐标（刚体建在原点 0）
    for (const auto& b : cmd.blocks) state_.colliders().erase(b.pos);
    // Jolt 侧同步：这些方块从静态世界移出 → 重建受影响的 chunk 形状
    {
        std::unordered_set<engine::shared::ChunkPos> touched;
        for (const auto& b : cmd.blocks) touched.insert(engine::shared::toChunkOf(b.pos));
        for (const auto& cp : touched) rebuildChunkStatic(cp.x, cp.y, cp.z);
    }
    // 碰撞箱不再手算：Jolt 接入时由 MutableCompoundShape 的 AddShape 生成。
    state_.bodies().push_back(body);
    state_.bindStructure(body.structureId, body.bodyId);
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
        worldBlocks.reserve(it->blocks.size());
        for (const auto& b : it->blocks) {
            const engine::shared::BlockPos world{
                static_cast<int>(std::llround(it->x + b.pos.x)),
                static_cast<int>(std::llround(it->y + b.pos.y)),
                static_cast<int>(std::llround(it->z + b.pos.z))};
            state_.colliders()[world] = b.type;
            worldBlocks.push_back(engine::shared::Block{b.type, world});
        }
        state_.unbindStructure(it->structureId);
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
    // ---- 刚体积分：已删除 ----
    for (auto& b : state_.bodies()) {
        if (b.motion != MotionType::Kinematic) continue;
        // 插值快照：先记下本 tick 起点的位置（= 上一 tick 的终点）。
        const bool   isPlayerBody = (b.bodyId == state_.playerBodyId() && playerState_ != nullptr);
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
           << "  生效系数=" << tm_.effectiveScale(engine::core::Domain::Physics);
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

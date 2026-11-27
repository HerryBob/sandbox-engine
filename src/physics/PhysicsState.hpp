#pragma once
#include "shared/block/Block.hpp"

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace engine::physics {

// 物理体运动类型（对应 Jolt 的 RigidBodyType）。
// Static 永不移动（地形，当前不进 bodies）；Dynamic 按速度积分并做碰撞回退（方块组）；
// Kinematic 速度由外部设定、受重力、位置由碰撞修正（生物/玩家）。
enum class MotionType { Static, Dynamic, Kinematic };

// 一个物理体（Jolt 刚体在数据层的对应物）。
//
// 与旧 RigidBody 的区别：多了 **bodyId / structureId 二分**。
//   bodyId      —— 物理体自身 id（物理线程分配，单调递增，不复用）
//   structureId —— 所属**结构** id（= 协议里的 contraptionId）；-1 = 不属于任何结构（玩家等）
// 1.0 一个结构只有一个刚体，两者数值相同；多刚体（约束装配）阶段结构 id 会有自己的分配器，
// 那时一个 structureId 对应 N 个 bodyId。
//
// 位置用 double（精度设计见架构文档 2.8：复合体=基准+局部坐标）。
struct PhysicsBody {
    int        bodyId = 0;                  // 物理体 id（物理线程分配）
    int        structureId = -1;            // 所属结构 id（-1 = 非结构体）
    MotionType motion = MotionType::Dynamic; // 方块组=Dynamic，生物=Kinematic
    double x = 0.0, y = 0.0, z = 0.0;       // 世界位置（基准；生物为脚底中心）
    double vx = 0.0, vy = 0.0, vz = 0.0;    // 速度（积分用）
    // 姿态（四元数）。当前只在打包时设一个固定值，用于验证"旋转数据通路"；
    // Jolt 接入后由刚体的真实角速度积分得到（届时删掉打包处那行临时赋值）。
    float  qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
    int    blockCount = 0;                  // 打包的方块数
    // 内部方块（局部坐标 + 类型）：世界坐标 = 基准 + 局部坐标（架构 2.8）。
    std::vector<engine::shared::Block> blocks;
    // 碰撞体半宽 / 中心偏移（相对基准）。
    // 求解器删除后这两组字段不再是"手工 AABB"：
    //   - 生物：作为 BoxShape 的半尺寸（装配期由 addPlayerBody 传入）
    //   - 刚体：Jolt 的 compound shape 自己算，这两组字段届时可删
    double halfExtentX = 0.0, halfExtentY = 0.0, halfExtentZ = 0.0;
    double centerOffsetX = 0.0, centerOffsetY = 0.0, centerOffsetZ = 0.0;
    // 踩地标志（求解器删除后恒为 false；Jolt 接入后由 CharacterVirtual::GetGroundState 取代）。
    bool   onGround = false;
};

// 两个物理体之间的关节约束（决定「多刚体 + 约束表」，见 09 §九 决策 1）。
// v1 只定义数据结构、约束表保持为空 —— 求解器侧还没有人读它。
struct PhysicsConstraint {
    int    id = 0;
    int    bodyA = -1;                      // 物理体 id（不是结构 id）
    int    bodyB = -1;
    double axisX = 0.0, axisY = 1.0, axisZ = 0.0; // 铰链轴（约定见 Jolt 接入时定）
    double anchorAX = 0.0, anchorAY = 0.0, anchorAZ = 0.0; // A 的局部锚点
    double anchorBX = 0.0, anchorBY = 0.0, anchorBZ = 0.0; // B 的局部锚点
    double minAngle = 0.0, maxAngle = 0.0;  // 角度限制（弧度）
    double damping = 0.0;                   // 阻尼
};

// 物理线程的**数据层**：只持有世界状态 + 分配 id + 只读统计。
//
// 分工与业务线程一一对应（规则5「数据层/逻辑层分开」）：
//      PhysicsState  ≙  business::World          —— 容器、定位、查询
//      PhysicsWorld  ≙  business::BusinessWorld  —— 推进、指令消费、回执组装
//
// **本类不含任何行为**：不积分、不求解、不产回执、不认识队列。
//
// 线程模型：纯物理线程独占（单写者，无锁）；对 main 暴露的是 PhysicsWorld 的原子投影。
class PhysicsState {
public:
    // ---- 刚体表 ----
    std::vector<PhysicsBody>&       bodies() noexcept       { return bodies_; }
    const std::vector<PhysicsBody>& bodies() const noexcept { return bodies_; }

    // 刚体 id 只由物理线程分配（单调递增，不复用）。
    int allocateBodyId() noexcept { return static_cast<int>(nextBodyId_++); }
    int allocateConstraintId() noexcept { return static_cast<int>(nextConstraintId_++); }

    // ---- 按 id / 结构 id 定位 ----
    PhysicsBody* findBodyById(int bodyId) noexcept {
        for (auto& b : bodies_) if (b.bodyId == bodyId) return &b;
        return nullptr;
    }
    const PhysicsBody* findBodyById(int bodyId) const noexcept {
        for (const auto& b : bodies_) if (b.bodyId == bodyId) return &b;
        return nullptr;
    }
    // 1.0 一个结构一个刚体：直接线性找（结构体数量很少）。
    PhysicsBody* findBodyByStructure(int structureId) noexcept {
        for (auto& b : bodies_) if (b.structureId == structureId) return &b;
        return nullptr;
    }

    // ---- 结构 → 刚体表（多刚体用）----
    // 1.0 表里每个结构只有一条；接口先立着，多刚体阶段不改调用方。
    void bindStructure(int structureId, int bodyId) {
        structureBodies_[structureId].push_back(bodyId);
    }
    void unbindStructure(int structureId) { structureBodies_.erase(structureId); }
    // 返回 nullptr 表示该结构没有刚体（或已解包）。
    const std::vector<int>* bodiesOfStructure(int structureId) const noexcept {
        const auto it = structureBodies_.find(structureId);
        return it == structureBodies_.end() ? nullptr : &it->second;
    }
    std::size_t structureCount() const noexcept { return structureBodies_.size(); }

    // ---- 约束表（v1 为空）----
    std::vector<PhysicsConstraint>&       constraints() noexcept       { return constraints_; }
    const std::vector<PhysicsConstraint>& constraints() const noexcept { return constraints_; }

    // ---- 静态方块碰撞体集合 ----
    // 求解器删除后它只是"静态世界的影子"（没有碰撞消费方），保留是为了让
    // SyncBlockSet / Place / Remove 的指令语义完整；Jolt 接入后由
    // "每 chunk 一个 StaticCompoundShape" 取代。
    std::unordered_map<engine::shared::BlockPos, engine::shared::BlockType>& colliders() noexcept {
        return colliders_;
    }
    const std::unordered_map<engine::shared::BlockPos, engine::shared::BlockType>&
    colliders() const noexcept { return colliders_; }

    // ---- 玩家体 ----
    int  playerBodyId() const noexcept { return playerBodyId_; }   // -1 = 未创建
    void setPlayerBodyId(int id) noexcept { playerBodyId_ = id; }

private:
    std::vector<PhysicsBody>       bodies_;
    std::unordered_map<int, std::vector<int>> structureBodies_; // 结构 id → 物理体 id 列表
    std::vector<PhysicsConstraint> constraints_;
    long long              nextBodyId_ = 1;        // 刚体 ID 分配器
    long long              nextConstraintId_ = 1;  // 约束 ID 分配器
    std::unordered_map<engine::shared::BlockPos, engine::shared::BlockType> colliders_;
    int                    playerBodyId_ = -1;     // 玩家体 id（-1 = 未创建）
};

} // namespace engine::physics

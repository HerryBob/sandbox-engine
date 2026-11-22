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

// 原型最小刚体（Contraption 打包产物）。
// 位置用 double（精度设计见架构文档 2.8：复合体=基准+局部坐标）。
struct RigidBody {
    int    id = 0;              // 刚体 ID（物理分配）
    MotionType motion = MotionType::Dynamic; // 运动类型：方块组=Dynamic，生物=Kinematic
    double x = 0.0, y = 0.0, z = 0.0; // 世界位置（基准；生物为脚底中心）
    double vx = 0.0, vy = 0.0, vz = 0.0; // 速度（2.3 积分用）
    // 姿态（四元数）。当前只在打包时设一个固定值，用于验证"旋转数据通路"；
    // Jolt 接入后由刚体的真实角速度积分得到（届时删掉打包处那行临时赋值）。
    float  qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
    int    blockCount = 0;      // 打包的方块数
    // 内部方块（局部坐标 + 类型）：世界坐标 = 基准 + 局部坐标（架构 2.8）。
    // 解包时按基准偏移回写静态碰撞体集合。
    std::vector<engine::shared::Block> blocks;
    // 碰撞体半宽 / 中心偏移（相对基准）。
    // 求解器删除后这两组字段不再是"手工 AABB"：
    //   - 生物：作为 CapsuleShape 的尺寸（装配期由 addPlayerBody 传入）
    //   - 刚体：Jolt 的 compound shape 自己算，这两组字段届时可删
    double halfExtentX = 0.0, halfExtentY = 0.0, halfExtentZ = 0.0;
    double centerOffsetX = 0.0, centerOffsetY = 0.0, centerOffsetZ = 0.0;
    // 踩地标志（求解器删除后恒为 false；Jolt 接入后由 CharacterVirtual::GetGroundState 取代）。
    bool   onGround = false;
};

// 物理线程的**数据层**：只持有世界状态 + 分配 id + 只读统计。
//
// 分工与业务线程一一对应（规则5「数据层/逻辑层分开」）：
//      PhysicsState  ≙  business::World          —— 容器、定位、查询
//      PhysicsWorld  ≙  business::BusinessWorld  —— 推进、指令消费、回执组装
//
// **本类不含任何行为**：不积分、不求解、不产回执、不认识队列。
// 为什么先分层再接 Jolt：换求解器只改逻辑层；
// 这里将来只需把 bodies_ 的元素换成 JPH::BodyID + 位置缓存，接口基本不动。
//
// 线程模型：纯物理线程独占（单写者，无锁）；对 main 暴露的是 PhysicsWorld 的原子投影。
class PhysicsState {
public:
    // ---- 刚体表 ----
    std::vector<RigidBody>&       bodies() noexcept       { return bodies_; }
    const std::vector<RigidBody>& bodies() const noexcept { return bodies_; }
    // 刚体 id 只由物理线程分配（单调递增，不复用）。
    int allocateBodyId() noexcept { return static_cast<int>(nextBodyId_++); }

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
    std::vector<RigidBody> bodies_;
    long long              nextBodyId_ = 1;    // 刚体 ID 分配器
    std::unordered_map<engine::shared::BlockPos, engine::shared::BlockType> colliders_;
    int                    playerBodyId_ = -1; // 玩家体 id（-1 = 未创建）
};

} // namespace engine::physics

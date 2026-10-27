#pragma once

#include "shared/block/Block.hpp"

#include <vector>

namespace engine::shared {

// 业务 → 物理：业务 Tick 边界下发的指令。
// 原型指令：MoveBlock（移动演示）+ SyncBlockSet（全量同步方块→碰撞体）
//          + PlaceBlock/RemoveBlock（放置/破坏，业务裁决通过后下发）
//          + Pack/UnpackContraption（打包/解包）。
struct PhysicsCommand {
    enum class Type {
        MoveBlock,         // 移动演示方块（原型）
        SyncBlockSet,      // 全量同步业务世界方块 → 物理碰撞体集合（初始/重建）
        PlaceBlock,        // 放置一块方块（业务裁决通过 → 物理建碰撞体）
        RemoveBlock,       // 破坏一块方块（业务裁决通过 → 物理删碰撞体）
        PackContraption,   // 打包：方块列表转移进刚体（物理形态转移，见架构 2.7）
        UnpackContraption, // 解包：刚体方块回写静态碰撞体集合
    };
    Type type = Type::MoveBlock;
    int x = 0, y = 0, z = 0;    // 目标方块坐标（PlaceBlock/RemoveBlock）。
    // 目标域语义：contraptionId == -1 → 主世界（x/y/z 世界坐标，静态碰撞体）；
    //             contraptionId >= 0 → 刚体（x/y/z 为刚体局部坐标，飞船参考系）。
    int dx = 0, dy = 0, dz = 0; // 移动增量（MoveBlock）
    int blockType = 0;          // 放置的方块类型（PlaceBlock，BlockType 底层值）

    // Contraption 字段（打包/解包用）：
    int    contraptionId = -1;  // 目标刚体 ID（PlaceBlock/RemoveBlock 目标域；Unpack 目标刚体）
    int    blockCount = 0;      // 打包：方块数
    double ivx = 0.0, ivy = 0.0, ivz = 0.0; // 打包初始速度（业务裁决起飞速度）
    std::vector<Block> blocks;  // SyncBlockSet：全量方块；Pack：待打包方块（世界坐标）
};

// 物理 → 业务：刚体位置/姿态/碰撞标记回传。
// 原型结果：BlockMoved（演示方块位置快照）+ ContraptionPacked/Unpacked（打包回执）
//           + RigidBodyPos（刚体位置快照）+ BlockSetSize（碰撞体数量快照）
//           + CollisionEvent（刚体撞上静态方块的碰撞事件）。
struct PhysicsResult {
    enum class Type {
        BlockMoved, ContraptionPacked, ContraptionUnpacked, RigidBodyPos,
        BlockSetSize, CollisionEvent
    };
    Type       type = Type::BlockMoved;
    int        x = 0, y = 0, z = 0;        // 移动后坐标
    long long  processedCommands = 0;      // 物理侧累计已处理指令数
    double     simTime = 0.0;              // 物理侧当前 sim_time，供业务比对
    long long  blockSetSize = 0;           // BlockSetSize：物理侧静态碰撞体数量快照

    // Contraption 字段（回执/刚体快照用）：
    int        contraptionId = 0;          // 刚体 ID（打包完成/解包完成/刚体位置）
    int        blockCount = 0;             // 打包的方块数
    double     rx = 0.0, ry = 0.0, rz = 0.0; // 刚体世界位置（RigidBodyPos 用）

    // 碰撞事件字段（CollisionEvent 用）：
    int        cx = 0, cy = 0, cz = 0;     // 被撞静态方块的坐标
    int        nx = 0, ny = 0, nz = 0;     // 碰撞法向（刚体被推回的方向）
    double     impact = 0.0;               // 碰撞瞬间刚体速度（冲击强度）

    // 解包回执（ContraptionUnpacked 用）：世界坐标方块列表。
    // 业务据此重建 blocks_，对齐两侧坐标（解包闭环，架构 2.7）。
    std::vector<engine::shared::Block> blocks;
};

// 业务 → 渲染：变更事件流（数据层 ② 第四项，渲染快照正式化）。
// 每业务 tick 聚合一条：本 tick 变脏的 chunk 集合。渲染端（第 4 轨只读）
// 消费后标记 chunk 脏 → 重建该 chunk 的 mesh（未来 LOD 线程）。
// 与位置快照的区别：位置是"状态"（合并语义，中间态丢弃），
// 变更事件是"事件流"（增量：渲染端只重建变过的 chunk，不全量重画）。
// 刚体域方块变更不进此流（复合体 = 动态渲染单元，状态由 RigidBodyPos 驱动）。
struct BlockChangeEvent {
    long long tick = 0;                              // 业务 tick 序号（事件时序）
    std::vector<engine::shared::ChunkPos> dirtyChunks; // 本 tick 变脏的 chunk（去重）
};

} // namespace engine::shared

#pragma once

#include "shared/chunk/ChunkPos.hpp"
#include "shared/block/Block.hpp"
#include <vector>

namespace engine::shared {

// B55：本文件承载**五套**跨线程消息类型，按方向与语义分组（改动时注意别混）：
//   ① PhysicsCommand        业务 → 物理（指令）
//   ② PhysicsResult         物理 → 业务（回执/快照/碰撞事件）
//   ③ BodyStructureEvent    业务 → 渲染（刚体结构：事件流，不能丢）
//   ④ BodyPose              业务 → 渲染（刚体位姿：状态，中间帧可丢）
//   ⑤ BlockChangeEvent      业务 → 渲染（区块变更：事件流，增量）
// 共同约束：只用定宽类型、无 STL 容器以外的隐式结构（将来若要跨 DLL 传，需再收窄）。

// 业务 → 物理：业务 Tick 边界下发的指令。
// 原型指令：MoveBlock（移动演示）+ SyncBlockSet（全量同步方块→碰撞体）
//          + PlaceBlock/RemoveBlock（放置/破坏，业务裁决通过后下发）
//          + Pack/UnpackContraption（打包/解包）。
struct PhysicsCommand {
    enum class Type {
        MoveBlock,         // 移动演示方块（原型）
        SyncBlockSet,      // 全量同步业务世界方块 → 物理碰撞体集合（初始/重建）
        SyncChunks,        // 增量同步：只发变化的 chunk（流式加载/卸载时用）
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
    // B57：MoveBlock 只带增量、不带起点 —— 指令不可重放（丢了就丢了）。
    //      当前 MoveBlock 已是遗留路径（演示方块改用放置/破坏驱动），保持原样。
    int blockType = 0;          // 放置的方块类型（PlaceBlock，BlockType 底层值）

    // Contraption 字段（打包/解包用）：
    int    contraptionId = -1;  // 目标刚体 ID（PlaceBlock/RemoveBlock 目标域；Unpack 目标刚体）
    int    blockCount = 0;      // 打包：方块数
    double ivx = 0.0, ivy = 0.0, ivz = 0.0; // 打包初始速度（业务裁决起飞速度）
    std::vector<Block> blocks;  // SyncBlockSet：全量方块；SyncChunks：变化 chunk 的方块
                                // Pack：待打包方块（世界坐标）
    // SyncChunks：本次同步覆盖的 chunk 列表。物理侧按它"先清该 chunk 区域、再填 blocks"，
    // 所以列表里可以有"已卸载、blocks 里没有内容"的 chunk（清完即为空）。
    // 为什么不用全量：流式每加载一批就全量重发 8 万方块，物理侧要整表重建（实测 11~14ms/次），
    // 那一帧直接变成 30~70ms —— 就是"每次更新区块卡一下"。
    std::vector<engine::shared::ChunkPos> changedChunks;
};

// 物理 → 业务：刚体位置/姿态/碰撞标记回传。
// 原型结果：BlockMoved（演示方块位置快照）+ ContraptionPacked/Unpacked（打包回执）
//           + RigidBodyPos（刚体位置快照）+ BlockSetSize（碰撞体数量快照）
//           + CollisionEvent（刚体撞上静态方块的碰撞事件）。
//
// B58（已评估，本轮不改）：整个结构约 200B、20+ 字段，承载 6 种消息，队列 64 深
//   → 常驻约 13KB。原型阶段用"一个胖结构 + type 分支"换简单；将来按消息类型拆分
//   可省一个数量级带宽（已记入 性能问题记录 P12）。
struct PhysicsResult {
    enum class Type {
        BlockMoved, ContraptionPacked, ContraptionUnpacked, RigidBodyPos,
        BlockSetSize, CollisionEvent
    };
    Type       type = Type::BlockMoved;
    int        x = 0, y = 0, z = 0;        // BlockMoved：移动后坐标
    // B54：下面两个是**跨消息的同步校验信息**，目前只在 BlockMoved / BlockSetSize 上
    // 被读取（PhysicsBridge 用来比对"物理处理到第几条指令"，以及 sim 时间是否落后）。
    // 其它 type 也会带着它们发（结构体是共用的），但没人读 —— 不要据此推断语义。
    long long  processedCommands = 0;      // 物理侧累计已处理指令数
    double     simTime = 0.0;              // 物理侧当前 sim_time，供业务比对
    long long  blockSetSize = 0;           // BlockSetSize：物理侧静态碰撞体数量快照

    // Contraption 字段（回执/刚体快照用）：
    // 默认 -1 = "不属于任何刚体"，与 PhysicsCommand 的同名字段保持一致
    // （B52：原来默认 0，而 0 是合法的刚体 id，未赋值的回执会被误认成"刚体 0 的消息"）。
    int        contraptionId = -1;         // 刚体 ID（打包完成/解包完成/刚体位置）
    int        blockCount = 0;             // 打包的方块数
    double     rx = 0.0, ry = 0.0, rz = 0.0; // 刚体世界位置（RigidBodyPos 用）
    // 刚体旋转（四元数，RigidBodyPos / ContraptionPacked 用）。默认单位四元数 = 无旋转。
    // 渲染端的刚体顶点是局部坐标，model matrix = translate(pos) × mat4_cast(quat)。
    float      qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;

    // 碰撞事件字段（CollisionEvent 用）：
    int        cx = 0, cy = 0, cz = 0;     // 被撞静态方块的坐标
    // B56：法向是**整数轴向量**，每个分量只取 {-1, 0, 1}（碰撞发生在轴对齐方块面上）。
    int        nx = 0, ny = 0, nz = 0;     // 碰撞法向（刚体被推回的方向）
    double     impact = 0.0;               // 碰撞瞬间刚体速度（冲击强度）

    // 解包/打包回执（ContraptionUnpacked / ContraptionPacked 用）：
    // Unpacked = 世界坐标方块列表（业务据此重建 blocks_）；
    // Packed   = 刚体局部坐标方块列表（业务据此交给渲染端建网格，不必自己留副本）。
    std::vector<Block> blocks;   // B53：与文件内其它声明统一定宽
};

// 业务 → 渲染：刚体结构事件（打包/解包）。
// 事件流语义：逐条送达、不能丢（丢一条 = 渲染端多一个或漏一个网格）。
struct BodyStructureEvent {
    int  contraptionId = -1;  // 刚体 ID（渲染端 bodyMeshes_ 的 key）
    bool destroy = false;     // true=解包销毁网格；false=打包建网格
    std::vector<engine::shared::Block> blocks; // 局部坐标方块列表（destroy 时为空）
};

// 业务 → 渲染：刚体位姿。
// 状态语义：中间帧可丢，渲染端只取最新一条（旧位置没有留存价值）。
struct BodyPose {
    int    contraptionId = -1;
    double x = 0.0, y = 0.0, z = 0.0; // 刚体世界位置
    // 刚体旋转（四元数；默认单位 = 无旋转）。与 PhysicsResult 同名字段一一对应。
    float  qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
};

// 业务 → 渲染：变更事件流（数据层 ② 第四项，渲染快照正式化）。
// 每业务 tick 聚合一条：本 tick 变脏的 chunk 集合。渲染端（第 4 轨只读）
// 消费后标记 chunk 脏 → 重建该 chunk 的 mesh（未来 LOD 线程）。
// 与位置快照的区别：位置是"状态"（合并语义，中间态丢弃），
// 变更事件是"事件流"（增量：渲染端只重建变过的 chunk，不全量重画）。
// 刚体域方块变更不进此流（复合体 = 动态渲染单元，状态由 RigidBodyPos 驱动）。
struct BlockChangeEvent {
    long long tick = 0;                              // 业务 tick 序号（事件时序）
    // B113：tick 目前**只写不读** —— 生产端会填，渲染端不消费它。
    //        保留是为了将来做"事件时序诊断/去重"时不用改协议；不要假设它被用于排序。
    std::vector<engine::shared::ChunkPos> dirtyChunks; // 本 tick 变脏的 chunk（去重）
};

} // namespace engine::shared

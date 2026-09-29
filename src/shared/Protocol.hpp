#pragma once

namespace engine::shared {

// 输入事件：输入线程墙钟采样，带时间戳，不受 time_scale 影响。
struct InputEvent {
    double wallTimestamp; // 墙钟秒（采集时刻）
    int    keyState;      // 原型占位：0=无操作，1=按住推力键
    bool   valid;         // 服务端输入使能：false=玩家当前不可操控，事件不施力
};

// 业务 → 物理：业务 Tick 边界下发的刚体指令。
// 原型指令：MoveBlock（移动演示方块）+ Pack/UnpackContraption（打包/解包）。
struct PhysicsCommand {
    enum class Type { MoveBlock, PackContraption, UnpackContraption };
    Type type = Type::MoveBlock;
    int x = 0, y = 0, z = 0;    // 目标方块坐标
    int dx = 0, dy = 0, dz = 0; // 移动增量

    // Contraption 字段（打包/解包用）：
    int    contraptionId = 0;      // 打包：0（物理新分配）；解包：目标刚体 ID
    int    blockCount = 0;         // 打包：方块数
    double ivx = 0.0, ivy = 0.0, ivz = 0.0; // 打包初始速度（业务裁决起飞速度）
};

// 物理 → 业务：刚体位置/姿态/碰撞标记回传。
// 原型结果：BlockMoved（演示方块位置快照）+ ContraptionPacked/Unpacked（打包回执）
//           + RigidBodyPos（刚体位置快照，飞船在动）。
struct PhysicsResult {
    enum class Type { BlockMoved, ContraptionPacked, ContraptionUnpacked, RigidBodyPos };
    Type       type = Type::BlockMoved;
    int        x = 0, y = 0, z = 0;        // 移动后坐标
    long long  processedCommands = 0;      // 物理侧累计已处理指令数
    double     simTime = 0.0;              // 物理侧当前 sim_time，供业务比对

    // Contraption 字段（回执/刚体快照用）：
    int        contraptionId = 0;          // 刚体 ID（打包完成/解包完成/刚体位置）
    int        blockCount = 0;             // 打包的方块数
    double     rx = 0.0, ry = 0.0, rz = 0.0; // 刚体世界位置（RigidBodyPos 用）
};

} // namespace engine::shared

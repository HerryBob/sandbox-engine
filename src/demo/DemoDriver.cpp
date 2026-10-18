#include "demo/DemoDriver.hpp"
#include <iostream>
namespace engine::demo {

DemoDriver::DemoDriver(engine::business::BusinessWorld& bw): bw_(bw) {}

void DemoDriver::step() {
    // ---- 方块放置/破坏演示（2.7：裁决各一次，模拟玩家操作）----
    // 2026-10-16 迁移：业务 20→25 TPS（dt 0.05→0.04），tick 阈值按 ×1.25
    // 放大，保持各演示事件的墙钟时刻与旧引擎一致（注释中括号为旧值）。
    // tick 1 ：全量同步初始世界 → 物理碰撞体集合
    // tick 37(30)：在 (0,5,0) 放置 1 块石头（合法 → 通过，自动排队 PlaceBlock）
    //          位置选在飞行路径外，避免干扰飞船演示。
    // tick 43(35)：再放 (0,5,0)（位置已占用 → 拒绝，不发指令）
    // tick 50(40)：破坏 (0,5,0)（存在 → 通过，自动排队 RemoveBlock）
    // tick 56(45)：破坏 (9,9,9)（不存在 → 拒绝）
    // ---- 飞船改造演示（3.4 演进：刚体内方块增删，物理增量重算碰撞箱）----
    // tick 100(80)：在刚体（飞船）上局部坐标 (2,0,0) 放置木头（右舷扩建一格）
    //          → 物理刚体 27→28 块，AABB 半宽 1.5→2.0、中心偏移 0→0.5
    // tick 200(160)：拆掉 (2,0,0)（存在 → 通过）→ 刚体 28→27 块，碰撞箱增量收缩
    // ---- 打包/解包演示 ----
    // tick 62(50)：发打包命令（把飞船区域 27 块打包成刚体，初始速度 + 推力起飞）
    // tick 312(250)：若已打包，发解包命令（拆回方块）——延后到 ~12.5s，
    //           让飞船飞过正常/减速/加速/域乘数/输入禁用多个演示阶段。
    const long long t = bw_.tickCount();
    if (t == 1) {
        bw_.syncAll();
    } else if (t == 37) {
        bw_.placeBlock(engine::shared::BlockPos{0, 5, 0},
                       engine::shared::BlockType::Stone, -1);
    } else if (t == 43) {
        bw_.placeBlock(engine::shared::BlockPos{0, 5, 0},
                       engine::shared::BlockType::Stone, -1); // 重复 → 拒绝
    } else if (t == 50) {
        bw_.removeBlock(engine::shared::BlockPos{0, 5, 0}, -1);
    } else if (t == 56) {
        bw_.removeBlock(engine::shared::BlockPos{9, 9, 9}, -1); // 不存在 → 拒绝
    } else if (t == 62) {
        bw_.packContraption();
    } else if (t == 100) {
        // 飞船改造：往刚体局部坐标 (2,0,0) 扩建一格（业务裁决：目标域=刚体）。
        bw_.placeBlock(engine::shared::BlockPos{2, 0, 0},
                       engine::shared::BlockType::Wood, bw_.packedId());
    } else if (t == 200) {
        // 飞船改造：拆掉刚体上 (2,0,0)（增量收缩碰撞箱）。
        bw_.removeBlock(engine::shared::BlockPos{2, 0, 0}, bw_.packedId());
    } else if (t == 312 && bw_.contraptionState() == 1) {
        bw_.unpackContraption();

    }
}

} // namespace engine::demo

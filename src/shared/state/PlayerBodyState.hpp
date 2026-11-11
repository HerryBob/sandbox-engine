#pragma once
#include <atomic>

namespace engine::shared {

// 玩家体状态（共享槽）：物理线程每 tick 写，输入线程读（玩家模式下相机跟随）。
// 位置语义与 RigidBody 一致：脚底中心，眼高偏移由读取方自行叠加。
struct PlayerBodyState {
    std::atomic<double> x{ 0.0 };
    std::atomic<double> y{ 0.0 };
    std::atomic<double> z{ 0.0 };
    // 朝向（弧度，yaw=0 朝 -Z，与相机 yaw 同一约定）。
    // 写者：输入线程（玩家模式下把相机 yaw 同步过来）；物理线程不碰它——
    // 朝向由鼠标输入驱动，不是物理量。
    std::atomic<double> yaw{ 0.0 };
    std::atomic<bool>   valid{ false }; // 物理线程尚未写过位置时为 false
};

} // namespace engine::shared

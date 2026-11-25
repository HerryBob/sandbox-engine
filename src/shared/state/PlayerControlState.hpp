#pragma once
#include <atomic>

namespace engine::shared {

// 玩家操控意图（共享槽）：渲染线程按当前按键写，物理线程每 tick 消费后灌进玩家体速度。
// 只表达"想怎么走"，不表达"在哪里"——位置由物理线程算完写回 PlayerBodyState。
struct PlayerControlState {
    std::atomic<double> vx{ 0.0 }; // 目标水平速度 x（格/秒，世界坐标）
    std::atomic<double> vz{ 0.0 }; // 目标水平速度 z（格/秒，世界坐标）
    std::atomic<bool>   jumpRequested{ false }; // 跳跃请求位（按下边沿置位，物理线程 exchange 消费）
};

} // namespace engine::shared

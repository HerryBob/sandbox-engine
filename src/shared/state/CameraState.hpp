#pragma once
#include <atomic>

namespace engine::shared {

// 相机状态（共享槽）：输入线程（InputSampler）写，渲染/业务线程只读。
// 拆自原 PlayerInput（收窄依赖面）：
//   · 渲染只需相机 5 字段 → 算 MVP（updateViewProj）；
//   · 业务只需相机 5 字段 + 点击 → DDA 射线；
//   · 点击事件（clickSeq/clickButton）单独归 PlayerInput（输入→业务专用通道）。
struct CameraState {
    // B47：与 TimeManager 保持一致的前提断言 —— 非 lock-free 平台上
    // atomic<double> 会退化成内部锁，而这个槽在输入线程 125Hz 写入路径上，
    // 还要求渲染线程无锁读取，必须是真正的无锁原子。
    static_assert(std::atomic<double>::is_always_lock_free,
                  "CameraState requires lock-free atomic<double>; "
                  "this platform's atomic<double> is not lock-free");

    std::atomic<double> camX{ 0.0 };
    std::atomic<double> camY{ 0.0 };
    std::atomic<double> camZ{ 0.0 };
    std::atomic<double> yaw{ 0.0 };   // 左右旋转（弧度）
    std::atomic<double> pitch{ 0.0 };  // 上下旋转（弧度）
    // 视角模式（输入线程写，渲染线程读）：
    //   0 = 自由相机（WASD 飞行）
    //   1 = 第一人称（相机在玩家眼位，自身模型需隐藏，否则视线被自己的头/躯干堵住）
    //   2 = 第三人称（相机沿视线反方向后退固定距离，能看到自己）
    std::atomic<int> viewMode{ 0 };
};

} // namespace engine::shared

#pragma once
#include <atomic>

namespace engine::shared {

// 相机状态（共享槽）：输入线程（InputSampler）写，渲染/业务线程只读。
// 拆自原 PlayerInput（收窄依赖面）：
//   · 渲染只需相机 5 字段 → 算 MVP（updateViewProj）；
//   · 业务只需相机 5 字段 + 点击 → DDA 射线；
//   · 点击事件（clickSeq/clickButton）单独归 PlayerInput（输入→业务专用通道）。
struct CameraState {
    std::atomic<double> camX{ 0.0 };
    std::atomic<double> camY{ 0.0 };
    std::atomic<double> camZ{ 0.0 };
    std::atomic<double> yaw{ 0.0 };   // 左右旋转（弧度）
    std::atomic<double> pitch{ 0.0 };  // 上下旋转（弧度）
};

} // namespace engine::shared

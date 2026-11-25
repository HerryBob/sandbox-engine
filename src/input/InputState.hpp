#pragma once
#include <atomic>

namespace engine::input {

// 渲染线程的**数据层**：键盘状态 + 待消费视角增量 + 模式位 + 玩家体尺寸/速度。
//
// 分工与其它线程一致（规则5「数据层/逻辑层分开」）：
//      InputState    ≙  business::World / physics::PhysicsState / worldgen::StreamState
//      InputSampler  ≙  business::BusinessWorld / physics::PhysicsWorld / ChunkStreamer
//
// **本类不含任何行为**：不采样、不算朝向、不决定相机模式怎么切。
// GLFW 回调只写这里（键盘/鼠标是"事件源"），InputSampler 的 tick 只读这里并产出
// CameraState / PlayerControlState —— 回调与逻辑之间不再直接耦合。
//
// 线程模型：
//   · 键盘原子 + 视角增量：**GLFW 回调（main 线程）写**，渲染线程读/取走 → 原子。
//   · 模式位 / spaceHeldPrev_ / 尺寸常量：渲染线程独占 → 普通字段。
class InputState {
public:
    // ---- 键盘（GLFW 回调写，输入逻辑读）----
    std::atomic<bool> keyW_{false}, keyS_{false}, keyA_{false}, keyD_{false};
    std::atomic<bool> keySpace_{false}, keyShift_{false}, keyR_{false};

    // ---- 待消费的视角增量（cursorPosCb 累加，tick 里 exchange(0) 取走）----
    std::atomic<double> yawDelta_{0.0};
    std::atomic<double> pitchDelta_{0.0};

    // ---- 相机模式（渲染线程独占）----
    // false = 自由相机（WASD 飞行），true = 玩家模式（位置跟随玩家体）。
    bool playerMode_ = false;
    // 玩家模式下的视角：false = 第一人称（相机在眼位），true = 第三人称（沿视线后退）。
    // 只在 playerMode_ 为 true 时有意义。
    bool thirdPerson_ = false;
    // 上一帧空格是否按下（边沿检测用：只在"刚按下"这一帧起跳）。
    bool spaceHeldPrev_ = false;

    // ---- 玩家体尺寸与速度（装配期由 setBodyMetrics 按实体类型覆盖）----
    // 默认值 MC 口径；用字段而非 constexpr —— 同一套采样要服务不同体型的实体。
    double eyeHeight_ = 1.62;
    double walkSpeed_ = 4.3;
};

} // namespace engine::input

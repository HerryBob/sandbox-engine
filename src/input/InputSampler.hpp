#pragma once
#include "shared/state/CameraState.hpp"
#include "shared/state/PlayerInput.hpp"

#include <atomic>
#include <chrono>

struct GLFWwindow; // 前向声明：回调签名只用到指针

namespace engine::input {

// 输入线程：真实键盘/鼠标输入（125Hz 采样，墙钟原生，不受 time_scale 影响）。
// 职责划分：
//   ① attach() 注册 GLFW 回调（键盘/鼠标）——回调在主线程 pollEvents 时触发，
//      只写原子状态（按键状态/视角增量/点击序号），不做相机运算；
//   ② 输入线程每 tick 调 sample()：消费回调增量 → 更新相机 → 写 CameraState；
//   ③ 鼠标点击回调直接写 PlayerInput（clickSeq/clickButton，业务线程消费）。
// 输出槽按职责拆分：
//   · CameraState：相机位置/视角（输入线程写；渲染算 MVP、业务做射线，只读）；
//   · PlayerInput：点击事件（鼠标回调写；业务消费做 DDA）。
// 单窗口单实例（引擎现状）：回调经静态 s_self_ 取本对象，多窗口再改查表。
class InputSampler {
public:
    InputSampler();

    // 绑定窗口与两个输出槽：注册键盘/鼠标回调 + 初始化光标基准。
    void attach(GLFWwindow* window,
                engine::shared::CameraState* camera,
                engine::shared::PlayerInput* clicks);

    // 输入线程每 tick 调用：消费视角增量 + 键盘状态 → 更新相机 → 写 CameraState。
    void sample();

    // ---- GLFW 回调（静态，单实例经 s_self_ 分发）----
    static void keyboardCb(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void mouseButtonCb(GLFWwindow* w, int button, int action, int mods);
    static void cursorPosCb(GLFWwindow* w, double x, double y);

private:
    void writeCameraState(); // 相机状态 → CameraState 原子槽

    // 回调经它找对象（单实例）。
    inline static InputSampler* s_self_ = nullptr;

    engine::shared::CameraState* cameraOut_ = nullptr;   // 相机输出槽（attach 后非空）
    engine::shared::PlayerInput* clickOut_ = nullptr;    // 点击输出槽（attach 后非空）
    GLFWwindow* window_ = nullptr;

    // ---- 回调写 / 输入线程读（原子）----
    std::atomic<bool> keyW_{false}, keyS_{false}, keyA_{false}, keyD_{false};
    std::atomic<bool> keySpace_{false}, keyShift_{false}, keyR_{false};
    std::atomic<double> yawDelta_{0.0};    // 待消费视角增量（cursorPosCb 累加）
    std::atomic<double> pitchDelta_{0.0};
    std::atomic<double> lastCursorX_{0.0}; // 光标上次位置（增量计算基准）
    std::atomic<double> lastCursorY_{0.0};

    // ---- 相机状态（输入线程独占写）----
    double camX_ = 0.0, camY_ = 1.5, camZ_ = 3.5; // 初始位置：站在 y=0 平台顶面（眼睛高 1.5）
    double yaw_ = 0.0;                            // 左右旋转（弧度）
    double pitch_ = 0.0;                          // 上下旋转（弧度，clamp ±83°）

    std::chrono::steady_clock::time_point lastLog_;
};

} // namespace engine::input

#include "input/InputSampler.hpp"
#include "core/Log.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace engine::input {

InputSampler::InputSampler()
    : lastLog_(std::chrono::steady_clock::now()) {}

void InputSampler::attach(GLFWwindow* window,
                          engine::shared::CameraState* camera,
                          engine::shared::PlayerInput* clicks) {
    window_ = window;
    cameraOut_ = camera;
    clickOut_ = clicks;
    s_self_ = this; // 单实例：回调经静态指针取本对象
    glfwSetKeyCallback(window, keyboardCb);
    glfwSetMouseButtonCallback(window, mouseButtonCb);
    glfwSetCursorPosCallback(window, cursorPosCb);
    // 初始化光标基准：避免首次回调 dx = x-0 导致视角跳变。
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    lastCursorX_.store(x, std::memory_order_relaxed);
    lastCursorY_.store(y, std::memory_order_relaxed);
    LOG_INFO("[输入] 键盘/鼠标回调已注册（真实输入）");
}

void InputSampler::sample() {
    // 消费视角增量（回调累加 / 本线程取走），clamp 俯仰防翻跟头。
    yaw_   += yawDelta_.exchange(0.0, std::memory_order_relaxed);
    pitch_ += pitchDelta_.exchange(0.0, std::memory_order_relaxed);
    pitch_ = std::clamp(pitch_, -1.45, 1.45); // ±83°

    // 键盘移动（速度 2.5 格/s，与原渲染线程逻辑一致；125Hz → dt=0.008）。
    const double speed = 2.5 * 0.008;
    const double fwdX = std::sin(yaw_),  fwdZ = -std::cos(yaw_); // 前方向
    const double rgtX = std::cos(yaw_),  rgtZ =  std::sin(yaw_); // 右方向
    if (keyW_.load(std::memory_order_relaxed)) {
        camX_ += fwdX * speed; camZ_ += fwdZ * speed;
    }
    if (keyS_.load(std::memory_order_relaxed)) {
        camX_ -= fwdX * speed; camZ_ -= fwdZ * speed;
    }
    if (keyD_.load(std::memory_order_relaxed)) {
        camX_ += rgtX * speed; camZ_ += rgtZ * speed;
    }
    if (keyA_.load(std::memory_order_relaxed)) {
        camX_ -= rgtX * speed; camZ_ -= rgtZ * speed;
    }
    if (keySpace_.load(std::memory_order_relaxed)) camY_ += speed;
    if (keyShift_.load(std::memory_order_relaxed)) camY_ -= speed;
    if (keyR_.load(std::memory_order_relaxed)) { // 重置视角（回出生点：平台上方）
        camX_ = 0.0; camY_ = 1.5; camZ_ = 3.5;
        yaw_ = 0.0; pitch_ = 0.0;
    }

    writeCameraState();
}

void InputSampler::writeCameraState() {
    if (!cameraOut_) return;
    cameraOut_->camX.store(camX_, std::memory_order_relaxed);
    cameraOut_->camY.store(camY_, std::memory_order_relaxed);
    cameraOut_->camZ.store(camZ_, std::memory_order_relaxed);
    cameraOut_->yaw.store(yaw_, std::memory_order_relaxed);
    cameraOut_->pitch.store(pitch_, std::memory_order_relaxed);
}

// 键盘回调（主线程 pollEvents 触发）：只记录按下/松开状态，不做事。
void InputSampler::keyboardCb(GLFWwindow* w, int key, int, int action, int) {
    auto* self = s_self_;
    if (!self || !w) return;
    if (action == GLFW_REPEAT) return; // 重复事件不改变状态
    const bool pressed = (action == GLFW_PRESS);
    switch (key) {
    case GLFW_KEY_W:        self->keyW_.store(pressed, std::memory_order_relaxed); break;
    case GLFW_KEY_S:        self->keyS_.store(pressed, std::memory_order_relaxed); break;
    case GLFW_KEY_A:        self->keyA_.store(pressed, std::memory_order_relaxed); break;
    case GLFW_KEY_D:        self->keyD_.store(pressed, std::memory_order_relaxed); break;
    case GLFW_KEY_SPACE:    self->keySpace_.store(pressed, std::memory_order_relaxed); break;
    case GLFW_KEY_LEFT_SHIFT: self->keyShift_.store(pressed, std::memory_order_relaxed); break;
    case GLFW_KEY_R:        self->keyR_.store(pressed, std::memory_order_relaxed); break;
    default: break;
    }
}

// 鼠标按键回调：按下边沿 → 写点击序号（业务线程消费做 DDA）。
void InputSampler::mouseButtonCb(GLFWwindow* w, int button, int action, int) {
    auto* self = s_self_;
    if (!self || !w || !self->clickOut_) return;
    if (action != GLFW_PRESS) return;
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        self->clickOut_->clickButton.store(0, std::memory_order_relaxed);
        self->clickOut_->clickSeq.fetch_add(1, std::memory_order_relaxed);
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        self->clickOut_->clickButton.store(1, std::memory_order_relaxed);
        self->clickOut_->clickSeq.fetch_add(1, std::memory_order_relaxed);
    }
}

// 鼠标移动回调：增量 × 灵敏度 → 累加视角增量（输入线程消费）。
void InputSampler::cursorPosCb(GLFWwindow* w, double x, double y) {
    auto* self = s_self_;
    if (!self || !w) return;
    constexpr double kSens = 0.005; // 拖动灵敏度（弧度/像素）
    const double dx = x - self->lastCursorX_.load(std::memory_order_relaxed);
    const double dy = y - self->lastCursorY_.load(std::memory_order_relaxed);
    self->yawDelta_.fetch_add(dx * kSens, std::memory_order_relaxed);
    self->pitchDelta_.fetch_add(-dy * kSens, std::memory_order_relaxed);
    self->lastCursorX_.store(x, std::memory_order_relaxed);
    self->lastCursorY_.store(y, std::memory_order_relaxed);
}

} // namespace engine::input

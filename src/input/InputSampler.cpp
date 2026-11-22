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
                          engine::shared::PlayerInput* clicks,
                          engine::shared::PlayerBodyState* player,
                          engine::shared::PlayerControlState* control) {
    window_ = window;
    cameraOut_ = camera;
    clickOut_ = clicks;
    playerOut_ = player;
    controlOut_ = control;
    s_self_ = this; // 单实例：回调经静态指针取本对象
    glfwSetKeyCallback(window, keyboardCb);
    glfwSetMouseButtonCallback(window, mouseButtonCb);
    glfwSetCursorPosCallback(window, cursorPosCb);
    // 初始化光标基准：避免首次回调 dx = x-0 导致视角跳变。
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    lastCursorX_.store(x, std::memory_order_relaxed);
    lastCursorY_.store(y, std::memory_order_relaxed);
    ENGINE_LOG_INFO("Input", "键盘/鼠标回调已注册（真实输入）");
}

bool InputSampler::consumeModeToggles() {
    bool modeChanged = false;

    // 模式切换请求（F4 按下时由键盘回调置位，本线程取走）：自由相机 ↔ 玩家跟随。
    if (clickOut_ != nullptr &&
        clickOut_->togglePlayerMode.exchange(false, std::memory_order_relaxed)) {
        state_.playerMode_ = !state_.playerMode_;
        modeChanged = true;
    }

    // 第一/第三人称切换（F5）：只在玩家模式下有意义，自由相机时忽略请求。
    if (clickOut_ != nullptr &&
        clickOut_->toggleThirdPerson.exchange(false, std::memory_order_relaxed)) {
        if (state_.playerMode_) {
            state_.thirdPerson_ = !state_.thirdPerson_;
            modeChanged = true;
        }
    }
    return modeChanged;
}

void InputSampler::updateLook() {
    // 消费视角增量（回调累加 / 本线程取走），clamp 俯仰防翻跟头。
    yaw_   += state_.yawDelta_.exchange(0.0, std::memory_order_relaxed);
    pitch_ += state_.pitchDelta_.exchange(0.0, std::memory_order_relaxed);
    pitch_ = std::clamp(pitch_, -1.45, 1.45); // ±83°
}

InputSampler::LookBasis InputSampler::lookBasis() const {
    // 朝向基向量（两种模式共用；yaw = 0 时朝 -Z）。
    LookBasis b{};
    b.fwdX = std::sin(yaw_);  b.fwdZ = -std::cos(yaw_);   // 前方向
    b.rgtX = std::cos(yaw_);  b.rgtZ =  std::sin(yaw_);   // 右方向
    return b;
}

bool InputSampler::spaceDown() const {
    return state_.keySpace_.load(std::memory_order_relaxed);
}

void InputSampler::updatePlayerCamera(const LookBasis& b) {
    // 玩家模式：位置锁在玩家体（脚底 + 眼高），视角仍归鼠标。
    if (playerOut_ != nullptr &&
        playerOut_->valid.load(std::memory_order_relaxed)) {
        // 插值读：本线程 125Hz 高于物理 25Hz，直接读 x/y/z 会把 40ms 阶梯原样
        // 采进相机位置，渲染线程再怎么高频采样也只是在采同一段阶梯。
        double px = 0.0, py = 0.0, pz = 0.0;
        playerOut_->readInterpolated(px, py, pz);
        if (state_.thirdPerson_) {
            // 第三人称：相机 = 瞄准点沿视线反方向后退固定距离。
            // 因为相机位置是从瞄准点沿 lookDir 反向推出来的，视线正好穿过瞄准点，
            // 玩家始终在画面中心，view 矩阵不用改（yaw/pitch 语义保持不变）。
            const double cp = std::cos(pitch_);
            const double sp = std::sin(pitch_);
            camX_ = px - b.fwdX * cp * kThirdPersonDist_;
            camY_ = py + kThirdPersonAim_ - sp * kThirdPersonDist_;
            camZ_ = pz - b.fwdZ * cp * kThirdPersonDist_;
        } else {
            // 第一人称：相机就在眼位。自身模型要由渲染侧隐藏，否则在头/躯干内部。
            camX_ = px;
            camY_ = py + state_.eyeHeight_;
            camZ_ = pz;
        }
    }
    if (state_.keyR_.load(std::memory_order_relaxed)) { // R：只重置朝向（位置归玩家体）
        yaw_ = spawnYaw_; pitch_ = spawnPitch_;
    }
}

void InputSampler::writeControlIntent(const LookBasis& b, bool space) {
    // 玩家模式：WASD 写操控意图（物理线程每 tick 取走当水平速度），空格改成跳跃。
    if (controlOut_ == nullptr) return;

    // WASD 先合成方向再归一化：斜向走不会比直走快。
    double dx = 0.0, dz = 0.0;
    if (state_.keyW_.load(std::memory_order_relaxed)) { dx += b.fwdX; dz += b.fwdZ; }
    if (state_.keyS_.load(std::memory_order_relaxed)) { dx -= b.fwdX; dz -= b.fwdZ; }
    if (state_.keyD_.load(std::memory_order_relaxed)) { dx += b.rgtX; dz += b.rgtZ; }
    if (state_.keyA_.load(std::memory_order_relaxed)) { dx -= b.rgtX; dz -= b.rgtZ; }
    const double len = std::sqrt(dx * dx + dz * dz);
    if (len > 1e-9) { dx /= len; dz /= len; }
    controlOut_->vx.store(dx * state_.walkSpeed_, std::memory_order_relaxed);
    controlOut_->vz.store(dz * state_.walkSpeed_, std::memory_order_relaxed);
    // 空格取按下边沿：按住不连跳。
    if (space && !state_.spaceHeldPrev_) {
        controlOut_->jumpRequested.store(true, std::memory_order_relaxed);
    }
}

void InputSampler::updateFreeCamera(const LookBasis& b, bool space) {
    // 自由相机模式：原逻辑（速度 8 格/s；125Hz → 每 tick 0.064 格）。
    // 这里空格仍是"按住上升"，不是跳跃。
    const double speed = 8 * 0.008;

    // 自由相机不驱动玩家体：必须每帧清零操控意图。
    // 否则从玩家模式切过来时，物理线程会继续读到上一帧残留的走速，人一直滑不停。
    if (controlOut_ != nullptr) {
        controlOut_->vx.store(0.0, std::memory_order_relaxed);
        controlOut_->vz.store(0.0, std::memory_order_relaxed);
    }

    if (state_.keyW_.load(std::memory_order_relaxed)) {
        camX_ += b.fwdX * speed; camZ_ += b.fwdZ * speed;
    }
    if (state_.keyS_.load(std::memory_order_relaxed)) {
        camX_ -= b.fwdX * speed; camZ_ -= b.fwdZ * speed;
    }
    if (state_.keyD_.load(std::memory_order_relaxed)) {
        camX_ += b.rgtX * speed; camZ_ += b.rgtZ * speed;
    }
    if (state_.keyA_.load(std::memory_order_relaxed)) {
        camX_ -= b.rgtX * speed; camZ_ -= b.rgtZ * speed;
    }
    if (space) camY_ += speed;
    if (state_.keyShift_.load(std::memory_order_relaxed)) camY_ -= speed;
    if (state_.keyR_.load(std::memory_order_relaxed)) { // 重置视角（回出生点：生成地形的地表上方）
        camX_ = spawnX_; camY_ = spawnY_; camZ_ = spawnZ_;
        yaw_ = spawnYaw_; pitch_ = spawnPitch_;
    }
}

void InputSampler::logModeChange(bool modeChanged) {
    if (!modeChanged) return;
    std::ostringstream os;
    if (state_.playerMode_) {
        os << "相机模式：玩家跟随 " << (state_.thirdPerson_ ? "第三人称" : "第一人称")
           << " 眼高=" << state_.eyeHeight_ << " 距离=" << (state_.thirdPerson_ ? kThirdPersonDist_ : 0.0)
           << " 相机=(" << camX_ << "," << camY_ << "," << camZ_ << ")";
    } else {
        os << "相机模式：自由相机 相机=(" << camX_ << "," << camY_ << "," << camZ_ << ")";
    }
    ENGINE_LOG_INFO("Input", os.str());
}

void InputSampler::tick() {
    const bool modeChanged = consumeModeToggles();   // ① F4/F5
    updateLook();                                    // ② 视角增量 + clamp

    const LookBasis b = lookBasis();                 // ③ 朝向基向量
    const bool space = spaceDown();

    if (state_.playerMode_) {
        updatePlayerCamera(b);                       // ④a 相机跟随玩家体

        // 朝向同步给玩家体：物理线程不写这个字段（朝向由鼠标驱动，不是物理量），
        // 渲染线程读它让模型整体转向。弧度、累加不清零，与相机 yaw 同一约定（yaw = 0 朝 -Z）。
        if (playerOut_ != nullptr) {
            playerOut_->yaw.store(yaw_, std::memory_order_relaxed);
        }

        writeControlIntent(b, space);                // ④c WASD → 操控意图 + 跳跃边沿
    } else {
        updateFreeCamera(b, space);                  // ⑤ 自由相机
    }

    writeCameraState();
    state_.spaceHeldPrev_ = space;
    logModeChange(modeChanged);
}

void InputSampler::setSpawn(double x, double y, double z, double yawRad, double pitchRad) {
    spawnX_ = x; spawnY_ = y; spawnZ_ = z;
    spawnYaw_ = yawRad; spawnPitch_ = pitchRad;
    camX_ = x; camY_ = y; camZ_ = z;
    yaw_ = yawRad; pitch_ = pitchRad;
    writeCameraState(); // 立即生效（渲染首帧前调用，避免首帧还在旧位置）
}

void InputSampler::setBodyMetrics(double eyeHeight, double walkSpeed) {
    if (eyeHeight > 0.0) state_.eyeHeight_ = eyeHeight;
    if (walkSpeed > 0.0) state_.walkSpeed_ = walkSpeed;
    ENGINE_LOG_INFO("Input", "体型数值注入：眼高=" + std::to_string(state_.eyeHeight_)
        + " 步速=" + std::to_string(state_.walkSpeed_));
}

void InputSampler::writeCameraState() {
    if (!cameraOut_) return;
    cameraOut_->camX.store(camX_, std::memory_order_relaxed);
    cameraOut_->camY.store(camY_, std::memory_order_relaxed);
    cameraOut_->camZ.store(camZ_, std::memory_order_relaxed);
    cameraOut_->yaw.store(yaw_, std::memory_order_relaxed);
    cameraOut_->pitch.store(pitch_, std::memory_order_relaxed);
    // 渲染线程据此决定要不要画自身模型（第一人称必须隐藏）。
    const int mode = !state_.playerMode_ ? 0 : (state_.thirdPerson_ ? 2 : 1);
    cameraOut_->viewMode.store(mode, std::memory_order_relaxed);
}

// 键盘回调（主线程 pollEvents 触发）：只记录按下/松开状态，不做事。
void InputSampler::keyboardCb(GLFWwindow* w, int key, int scancode, int action, int mods) {
    auto* self = s_self_;
    if (!self || !w) return;
    (void)scancode; (void)mods;   // 未使用（签名保留 GLFW 回调形参）

    // F3 单独处理：只认 GLFW_PRESS。
    //  ① auto-repeat（按住不放 / 系统按键状态未复位时的持续重发）一律是 GLFW_REPEAT，
    //     只认 PRESS 就不会被它反复触发；用"按住状态位"反而会在 RELEASE 丢失时失去保护。
    //  ② GLFW 在 Windows 上只有 lParam 的重复位被置上才报 REPEAT，真实按下仍是 PRESS，
    //     所以不会漏按。
    if (key == GLFW_KEY_F3) {
        if (action != GLFW_PRESS) return;   // RELEASE / REPEAT 都不触发
        if (self->clickOut_) {
            self->clickOut_->toggleHud.store(true, std::memory_order_relaxed);
        }
        return;
    }

    // F4 同上：只认按下边沿，模式状态由输入线程翻转。
    if (key == GLFW_KEY_F4) {
        if (action != GLFW_PRESS) return;
        if (self->clickOut_) {
            self->clickOut_->togglePlayerMode.store(true, std::memory_order_relaxed);
        }
        return;
    }

    // F5：玩家模式下切换第一/第三人称（自由相机模式下按了无效，直接忽略）。
    // 状态只在输入线程改，这里只置请求位。
    if (key == GLFW_KEY_F5) {
        if (action != GLFW_PRESS) return;
        if (self->clickOut_) {
            self->clickOut_->toggleThirdPerson.store(true, std::memory_order_relaxed);
        }
        return;
    }

    if (action == GLFW_REPEAT) return; // 其它键：重复事件不改变状态
    const bool pressed = (action == GLFW_PRESS);
    switch (key) {
    case GLFW_KEY_W:        self->state_.keyW_.store(pressed, std::memory_order_relaxed); break;
    case GLFW_KEY_S:        self->state_.keyS_.store(pressed, std::memory_order_relaxed); break;
    case GLFW_KEY_A:        self->state_.keyA_.store(pressed, std::memory_order_relaxed); break;
    case GLFW_KEY_D:        self->state_.keyD_.store(pressed, std::memory_order_relaxed); break;
    case GLFW_KEY_SPACE:    self->state_.keySpace_.store(pressed, std::memory_order_relaxed); break;
    case GLFW_KEY_LEFT_SHIFT: self->state_.keyShift_.store(pressed, std::memory_order_relaxed); break;
    case GLFW_KEY_R:        self->state_.keyR_.store(pressed, std::memory_order_relaxed); break;
    default: break;
    }
}

// 鼠标按键回调：按下边沿 → 写点击序号（业务线程消费做 DDA）。
void InputSampler::mouseButtonCb(GLFWwindow* w, int button, int action, int) {
    auto* self = s_self_;
    if (!self || !w || !self->clickOut_) return;
    if (action != GLFW_PRESS) return;
    // 一次按下 = 一条 ClickEvent 入队（配对与"不丢"由队列保证，不再拼两个原子变量）。
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        self->clickOut_->clicks.push(engine::shared::ClickEvent{ 0 });
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        self->clickOut_->clicks.push(engine::shared::ClickEvent{ 1 });
    }
}

// 鼠标移动回调：增量 × 灵敏度 → 累加视角增量（输入线程消费）。
void InputSampler::cursorPosCb(GLFWwindow* w, double x, double y) {
    auto* self = s_self_;
    if (!self || !w) return;
    constexpr double kSens = 0.005; // 拖动灵敏度（弧度/像素）
    const double dx = x - self->lastCursorX_.load(std::memory_order_relaxed);
    const double dy = y - self->lastCursorY_.load(std::memory_order_relaxed);
    self->lastCursorX_.store(x, std::memory_order_relaxed);
    self->lastCursorY_.store(y, std::memory_order_relaxed);
    // 窗口被移动/最大化后，光标相对窗口的坐标会整体跳一次：这么大的位移不可能是真拖动，
    // 只重设基准、不转视角，否则画面会突然甩到别处。
    constexpr double kJumpPx = 200.0;
    if (std::abs(dx) > kJumpPx || std::abs(dy) > kJumpPx) return;
    self->state_.yawDelta_.fetch_add(dx * kSens, std::memory_order_relaxed);
    self->state_.pitchDelta_.fetch_add(-dy * kSens, std::memory_order_relaxed);
}

} // namespace engine::input

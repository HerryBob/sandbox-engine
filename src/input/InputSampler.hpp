#pragma once
#include "input/InputState.hpp"
#include "shared/state/CameraState.hpp"
#include "shared/state/PlayerBodyState.hpp"
#include "shared/state/PlayerControlState.hpp"
#include "shared/state/PlayerInput.hpp"

#include <atomic>
#include <chrono>

struct GLFWwindow; // 前向声明：回调签名只用到指针

namespace engine::input {

// 输入线程：真实键盘/鼠标输入（125Hz 采样，墙钟原生，不受 time_scale 影响）。
// 职责划分：
//   ① attach() 注册 GLFW 回调（键盘/鼠标）——回调在主线程 pollEvents 时触发，
//      只写原子状态（按键状态/视角增量/点击序号），不做相机运算；
//   ② 输入线程每 tick 调 tick()：消费回调增量 → 更新相机 → 写 CameraState；
//   ③ 鼠标点击回调直接写 PlayerInput（clickSeq/clickButton，业务线程消费）。
// 输出槽按职责拆分：
//   · CameraState：相机位置/视角（输入线程写；渲染算 MVP、业务做射线，只读）；
//   · PlayerInput：点击事件（鼠标回调写；业务消费做 DDA）。
// 单窗口单实例（引擎现状）：回调经静态 s_self_ 取本对象，多窗口再改查表。
class InputSampler {
public:
    InputSampler();

    // 绑定窗口与三个输出槽 + 两个玩家槽：注册键盘/鼠标回调 + 初始化光标基准。
    // player  玩家体状态槽（F4 切到玩家模式后相机跟随它，可空）。
    // control 玩家操控意图槽（玩家模式下 WASD/空格写这里，物理线程读，可空）。
    void attach(GLFWwindow* window,
                engine::shared::CameraState* camera,
                engine::shared::PlayerInput* clicks,
                engine::shared::PlayerBodyState* player,
                engine::shared::PlayerControlState* control);

    // 输入线程每 tick 调用：消费视角增量 + 键盘状态 → 更新相机 → 写 CameraState。
    void tick();

    // 装配期设置出生点（生成地形后由 main 按实际地表高度设定；R 键回到这里）。
    // 同时覆盖当前位置，立即生效（渲染首帧前调用）。
    void setSpawn(double x, double y, double z, double yawRad, double pitchRad);

    // 按实体类型定义注入体型数值（装配期调用，输入线程未启动时安全）。
    // 只在值为正数时覆盖，无效值保留默认。
    void setBodyMetrics(double eyeHeight, double walkSpeed);

    // ---- GLFW 回调（静态，单实例经 s_self_ 分发）----
    static void keyboardCb(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void mouseButtonCb(GLFWwindow* w, int button, int action, int mods);
    static void cursorPosCb(GLFWwindow* w, double x, double y);

private:
    void writeCameraState(); // 相机状态 → CameraState 原子槽

    // ---- tick() 的六段（规则1 拆分）----
    // 朝向基向量（yaw = 0 时朝 -Z），两种模式共用
    struct LookBasis { double fwdX, fwdZ, rgtX, rgtZ; };

    bool consumeModeToggles();                      // ① F4/F5 请求（返回是否切换了模式）
    void updateLook();                              // ② 消费视角增量 + 俯仰 clamp
    LookBasis lookBasis() const;                    // ③ 由 yaw_ 算四个基向量
    void updatePlayerCamera(const LookBasis& b);    // ④a 相机跟随玩家体（第一/第三人称）
    void writeControlIntent(const LookBasis& b, bool space);  // ④c WASD → 操控意图 + 跳跃边沿
    void updateFreeCamera(const LookBasis& b, bool space);    // ⑤ 自由相机移动
    void logModeChange(bool modeChanged);           // 模式切换日志
    bool spaceDown() const;                         // 空格当前是否按下

    // 回调经它找对象（单实例）。
    inline static InputSampler* s_self_ = nullptr;

    engine::shared::CameraState* cameraOut_ = nullptr;   // 相机输出槽（attach 后非空）
    engine::shared::PlayerInput* clickOut_ = nullptr;    // 点击输出槽（attach 后非空）
    engine::shared::PlayerBodyState* playerOut_ = nullptr; // 玩家体状态槽（可空）
    engine::shared::PlayerControlState* controlOut_ = nullptr; // 玩家操控意图槽（可空）
    GLFWwindow* window_ = nullptr;

    // 数据层（InputState）：键盘原子 + 待消费视角增量 + 模式位 + 玩家体尺寸/速度。
    // GLFW 回调只写 state_，tick 只读 state_ —— 回调与逻辑之间不再直接耦合。
    InputState state_;

    // 第三人称相机的后退距离（格）与瞄准点抬高（格）—— 属**逻辑层调参**，不是世界状态。
    static constexpr double kThirdPersonDist_ = 4.0;
    static constexpr double kThirdPersonAim_ = 1.35;
    std::atomic<double> lastCursorX_{0.0}; // 光标上次位置（增量计算基准）
    std::atomic<double> lastCursorY_{0.0};

    // ---- 相机状态（输入线程独占写）----
    // 出生点：装配期由 main 按生成地形实际高度设定（不写死世界高度）；R 键回到这里。
    double spawnX_ = 0.0, spawnY_ = 1.5, spawnZ_ = 3.5;
    double spawnYaw_ = 0.0, spawnPitch_ = -0.35;

    double camX_ = 0.0, camY_ = 1.5, camZ_ = 3.5; // 当前位置（初始 = 出生点默认值）
    double yaw_ = 0.0;                            // 左右旋转（弧度）
    double pitch_ = -0.35;                        // 上下旋转（弧度，clamp ±83°；略低头看地形）

    std::chrono::steady_clock::time_point lastLog_;
};

} // namespace engine::input

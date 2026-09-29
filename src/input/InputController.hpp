#pragma once
#include <atomic>

namespace engine::input {

// 玩家操控权使能开关（服务端权威，见架构文档 2.5）。
// 与时间系数正交：时间系数管"世界流速"，enabled 管"玩家当前能否操控"。
// 禁用时输入线程照常墙钟采样，但事件标记为无效（不施加推力），
// 保留墙钟时间戳以便后续插值/联机对齐。
class InputController {
public:
    void setEnabled(bool e) noexcept {
        enabled_.store(e, std::memory_order_relaxed);
    }

    bool enabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> enabled_{true};
};

} // namespace engine::input

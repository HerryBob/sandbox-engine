#pragma once
#include "core/SPSCQueue.hpp"
#include "input/InputController.hpp"
#include "shared/Protocol.hpp"

#include <atomic>
#include <chrono>

namespace engine::input {

// 输入线程：墙钟原生采样，带时间戳入队，完全不受 time_scale 影响。
class InputSampler {
public:
    InputSampler();

    // 采样一次输入事件并压入队列（原型：恒定模拟"按住推力键"）。
    // 事件是否有效由 InputController 决定（玩家操控权，服务端权威）。
    void sample(double wallNow, const InputController& ctrl,
                engine::core::SPSCQueue<engine::shared::InputEvent>& q);

    // 油门开关（服务端权威，跨线程读）：true=按住推力键，false=松手滑行。
    void setThrottle(bool on);

    long long samples() const;

private:
    void maybeLog(double wallNow);

    std::atomic<long long> samples_{0};
    std::atomic<bool>      throttle_{true}; // 默认按住（演示加速）
    std::chrono::steady_clock::time_point lastLog_;
};

} // namespace engine::input

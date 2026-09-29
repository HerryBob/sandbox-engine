#include "input/InputSampler.hpp"
#include "core/Log.hpp"

#include <sstream>

namespace engine::input {

InputSampler::InputSampler()
    : lastLog_(std::chrono::steady_clock::now()) {}

void InputSampler::sample(double wallNow, const InputController& ctrl,
                          engine::core::SPSCQueue<engine::shared::InputEvent>& q) {
    // 原型：模拟"推力键"，keyState 由油门开关决定（按住/松手）。
    // valid 由服务端权威的 InputController 决定；禁用时事件仍采样但不可施力。
    engine::shared::InputEvent ev{wallNow,
                                  throttle_.load(std::memory_order_relaxed) ? 1 : 0,
                                  ctrl.enabled()};
    q.push(ev); // 满则丢弃本帧，原型可接受
    samples_.fetch_add(1, std::memory_order_relaxed);
    maybeLog(wallNow);
}

void InputSampler::setThrottle(bool on) {
    throttle_.store(on, std::memory_order_relaxed);
}

long long InputSampler::samples() const {
    return samples_.load(std::memory_order_relaxed);
}

void InputSampler::maybeLog(double wallNow) {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - lastLog_).count() >= 1.0) {
        lastLog_ = now;
        std::ostringstream os;
        os << "[输入] 墙钟=" << wallNow << "s 采样=" << samples()
           << "  (不受 time_scale 影响)";
        engine::core::log(engine::core::LogLevel::Info, os.str());
    }
}

} // namespace engine::input

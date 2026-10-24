#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace engine::core {

// 轻量线程封装：命名 + 生命周期管理。
// tick 节奏由传入的 body 自行控制（见各轨实现），body 内应周期检查 shouldStop()。
class ThreadLoop {
public:
    explicit ThreadLoop(std::string name) : name_(std::move(name)) {} //初始化和防止隐式转换
    ThreadLoop(const ThreadLoop&) = delete;
    ThreadLoop& operator=(const ThreadLoop&) = delete;
    ~ThreadLoop() { stop(); }
    //线程单一化 负责线程控制

    void start(std::function<void()> body) {
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread([this, body = std::move(body)] { body(); });
    }

    void stop() noexcept {
        running_.store(false, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
    }

    bool shouldStop() const noexcept {
        return !running_.load(std::memory_order_relaxed);
    }

    const std::string& name() const noexcept { return name_; }

private:
    std::string name_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace engine::core

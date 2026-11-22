#pragma once
#include "core/Log.hpp"

#include <atomic>
#include <exception>
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

    // 线程安全：start / stop 只能在**同一个线程**调用（通常是装配/收尾线程），
    // 不支持并发 start/stop（B14）。
    void start(std::function<void()> body) {
        // B9：重复 start() 会给 joinable 的 std::thread 赋值 → std::terminate。
        if (thread_.joinable()) {
            ENGINE_LOG_ERROR("Thread", "重复 start()：线程 " + name_ + " 已在运行，忽略");
            return;
        }
        // B10：空 body 会在新线程里抛 bad_function_call → std::terminate。
        if (!body) {
            ENGINE_LOG_ERROR("Thread", "start() 传入空 body：线程 " + name_ + "，忽略");
            return;
        }
        // 顺序不能反：running_ 必须在 body 可能开始执行之前就置真，
        // 否则新线程第一句 shouldStop() 可能读到 false 立刻退出。
        running_.store(true, std::memory_order_relaxed);
        try {
            // 线程入口登记日志用名字，日志行才能区分 business / physics / render 等。
            // lambda 捕获 this：name_ 的生命周期覆盖整个线程（stop() 先 join 再析构）。
            thread_ = std::thread([this, body = std::move(body)] {
                engine::core::logSetThreadName(name_.c_str());
                // B12：异常不允许穿出线程入口（穿出即调用 std::terminate）。
                try {
                    body();
                } catch (const std::exception& e) {
                    ENGINE_LOG_ERROR("Thread", "线程体抛异常（" + name_ + "）：" + e.what());
                } catch (...) {
                    ENGINE_LOG_ERROR("Thread", "线程体抛未知异常（" + name_ + "）");
                }
            });
        } catch (const std::exception& e) {
            // B13：std::thread 构造抛异常（资源不足）时，running_ 已经置真 ——
            // 不恢复就会留下"标志说在跑、实际没线程"的不一致状态，后续 stop() 行为异常。
            running_.store(false, std::memory_order_relaxed);
            ENGINE_LOG_ERROR("Thread", "启动线程失败（" + name_ + "）：" + e.what());
        }
    }

    void stop() noexcept {
        // B11/B15：self-join（在线程自己的 body 里调 stop）会在 noexcept 里抛
        // system_error → std::terminate。直接返回：线程会自然退出。
        if (thread_.get_id() == std::this_thread::get_id()) return;
        running_.store(false, std::memory_order_relaxed);
        if (thread_.joinable()) {
            // noexcept 里不允许异常穿出（join 只在非 joinable 时抛，此处已守卫，
            // 兜底再包一层，避免平台差异导致 terminate）。
            try {
                thread_.join();
            } catch (...) {
                ENGINE_LOG_ERROR("Thread", "join 失败：线程 " + name_);
            }
        }
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

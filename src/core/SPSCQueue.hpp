#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

namespace engine::core {

// 单生产者-单消费者（SPSC）无锁环形队列。
// 约束：push 只能被一个线程调用，pop 只能被一个线程调用。
// 原型单机模式：业务→物理、输入→物理 均为天然 SPSC，无需互斥锁。
template <typename T>
class SPSCQueue {
public:
    explicit SPSCQueue(std::size_t capacity = 1024)
        : capacity_(capacity), buffer_(capacity + 1) {}

    // 生产者：写入尾部。队列满时返回 false（调用方决定丢或重试）。
    bool push(const T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) % (capacity_ + 1);
        if (next == head_.load(std::memory_order_acquire)) {
            return false; // 满
        }
        buffer_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // 消费者：读头部。队列空时返回 false。
    bool pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false; // 空
        }
        out = buffer_[head];
        head_.store((head + 1) % (capacity_ + 1), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire)
            == tail_.load(std::memory_order_acquire);
    }

private:
    std::size_t capacity_;
    std::vector<T> buffer_;
    std::atomic<std::size_t> head_{0}; // 读位置（消费者）
    std::atomic<std::size_t> tail_{0}; // 写位置（生产者）
};

} // namespace engine::core

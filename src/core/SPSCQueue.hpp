#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace engine::core {

    // 单生产者-单消费者（SPSC）无锁环形队列。
    // 约束：push 只能被一个线程调用，pop 只能被一个线程调用；
    //       T 必须可默认构造，且移动赋值必须 noexcept（拷贝赋值允许抛）。
    template <typename T>
    class SPSCQueue {
        static_assert(std::is_nothrow_move_assignable_v<T>,
            "SPSCQueue requires T::operator=(T&&) noexcept");

    public:
        explicit SPSCQueue(std::size_t capacity = 1024)
            : capacity_(capacity), buffer_(capacity_ + 1) {
            assert(capacity > 0 && "SPSCQueue capacity must be > 0");
        }

        // 左值版本：调用方还要用 item，只能拷贝。拷贝可能抛（如 string 分配），
        // 因此这里不标 noexcept，让异常正常传播。
        bool push(const T& item) {
            const std::size_t tail = tail_.load(std::memory_order_relaxed);
            const std::size_t next = (tail + 1) % (capacity_ + 1);
            if (next == head_.load(std::memory_order_acquire)) {
                return false;
            }
            buffer_[tail] = item;              // 拷贝赋值
            tail_.store(next, std::memory_order_release);
            return true;
        }

        // 右值版本：移动赋值有 static_assert 保证 noexcept，可以安全标 noexcept。
        bool push(T&& item) noexcept {
            const std::size_t tail = tail_.load(std::memory_order_relaxed);
            const std::size_t next = (tail + 1) % (capacity_ + 1);
            if (next == head_.load(std::memory_order_acquire)) {
                return false;
            }
            buffer_[tail] = std::move(item);   // 移动赋值（noexcept）
            tail_.store(next, std::memory_order_release);
            return true;
        }

        // 移动赋值 noexcept → pop 可以标 noexcept。
        bool pop(T& out) noexcept {
            const std::size_t head = head_.load(std::memory_order_relaxed);
            if (head == tail_.load(std::memory_order_acquire)) {
                return false;
            }
            out = std::move(buffer_[head]);    // 移动赋值（noexcept）
            head_.store((head + 1) % (capacity_ + 1), std::memory_order_release);
            return true;
        }

        // 注意：空判定是快照，跨线程结果可能瞬间过时；生产/消费应依赖 push/pop 返回值。
        bool empty() const noexcept {
            return head_.load(std::memory_order_acquire)
                == tail_.load(std::memory_order_acquire);
        }

    private:
        static constexpr std::size_t kCacheLine = 128;

        std::size_t capacity_;
        std::vector<T> buffer_;

        alignas(kCacheLine) std::atomic<std::size_t> head_{ 0 };
        alignas(kCacheLine) std::atomic<std::size_t> tail_{ 0 };
    };

} // namespace engine::core
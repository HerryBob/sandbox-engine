#pragma once

#include "modapi/noise_api.h"

#include <cstdint>

namespace engine::worldgen {

// 噪声句柄（RAII 包装）：引擎侧唯一持有的"实例"形态。
//
// 为什么不是 std::unique_ptr<NoiseSource>：NoiseSource 是 C++ 虚类，跨 DLL
// 边界时虚表布局与 delete 归属都没有保证。这里只保存三样东西：
//   void* self           —— DLL 内 create 分配的实例
//   sample 函数指针      —— 每次采样一次间接跳转
//   destroy 函数指针     —— 释放动作发生在 DLL 内，与 create 对称
//
// 语义：可移动、不可拷贝（独占所有权，同 unique_ptr 的用法）。
// sample 是 const：底层实例必须无共享可变状态（生成线程并发调用）。
class NoiseHandle {
public:
    NoiseHandle() {}

    NoiseHandle(void* self, modapi::NoiseSampleFn sample, modapi::NoiseDestroyFn destroy)
        : self_(self), sample_(sample), destroy_(destroy) {}

    ~NoiseHandle() { reset(); }

    NoiseHandle(const NoiseHandle&) = delete;
    NoiseHandle& operator=(const NoiseHandle&) = delete;

    NoiseHandle(NoiseHandle&& other) noexcept
        : self_(other.self_), sample_(other.sample_), destroy_(other.destroy_) {
        other.self_ = nullptr;
        other.sample_ = nullptr;
        other.destroy_ = nullptr;
    }

    NoiseHandle& operator=(NoiseHandle&& other) noexcept {
        if (this != &other) {
            reset();
            self_ = other.self_;
            sample_ = other.sample_;
            destroy_ = other.destroy_;
            other.self_ = nullptr;
            other.sample_ = nullptr;
            other.destroy_ = nullptr;
        }
        return *this;
    }

    // 采样：句柄为空时返回 0（调用方应先检查 valid()）。
    double sample(double x, double z, std::uint64_t seed) const {
        return sample_ ? sample_(self_, x, z, seed) : 0.0;
    }

    bool valid() const { return self_ != nullptr && sample_ != nullptr; }

    // 手动释放：调 DLL 的 destroy 并清空三件套。
    void reset() {
        if (self_ != nullptr && destroy_ != nullptr) destroy_(self_);
        self_ = nullptr;
        sample_ = nullptr;
        destroy_ = nullptr;
    }

private:
    void*                  self_ = nullptr;
    modapi::NoiseSampleFn  sample_ = nullptr;
    modapi::NoiseDestroyFn destroy_ = nullptr;
};

} // namespace engine::worldgen

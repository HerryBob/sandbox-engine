#pragma once
#include <atomic>

namespace engine::core {

// 模拟域枚举：业务 / 物理
enum class Domain { Business, Physics };

// 全局时间缩放（time_scale）。
// 最终生效系数 = global（服务端权威，整个世界流速） × 域乘数（默认 1，调试/局部缩放用）。
// 生效对象：业务与物理两个模拟域；输入线程完全不读（墙钟原生）。
class TimeManager {
public:
    // 服务端权威：设置整个世界的时间流速（业务、物理一起缩放）。
    void setGlobal(double s) noexcept {
        global_.store(s, std::memory_order_relaxed);
    }
	//读取当前全局时间流速（默认 1.0）。
    double global() const noexcept {
        return global_.load(std::memory_order_relaxed);
    }

    // 单独调整某域的乘数（默认 1.0）。调试用：如单独慢放物理轨。
    void setDomainScale(Domain d, double s) noexcept {
        domainOf(d).store(s, std::memory_order_relaxed);
    }

    // 某域当前乘数（默认 1.0）。
    double domainScale(Domain d) const noexcept {
        return domainOf(d).load(std::memory_order_relaxed);
    }

    // 某域最终生效系数 = global × 域乘数。
    double effectiveScale(Domain d) const noexcept {
        return global_.load(std::memory_order_relaxed)
             * domainOf(d).load(std::memory_order_relaxed);
    }

private:
    const std::atomic<double>& domainOf(Domain d) const noexcept {
        return d == Domain::Physics ? physics_ : business_; 
    }
    std::atomic<double>& domainOf(Domain d) noexcept {
        return d == Domain::Physics ? physics_ : business_;
    }

    std::atomic<double> global_{1.0};
    std::atomic<double> business_{1.0};
    std::atomic<double> physics_{1.0};
};

} // namespace engine::core

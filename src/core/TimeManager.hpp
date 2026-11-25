#pragma once
#include <atomic>
#include <cmath>   // std::isfinite
#include <cstddef>

#include "core/AtomicGuarantees.hpp"

namespace engine::core {

    // 全局时间缩放（time_scale）—— **整个游戏只有一条时间线**。
    //
    // 历史：这里原来有 Domain{Business, Physics} + setDomainScale/domainScale/
    // effectiveScale(Domain)，允许分别缩放业务域与物理域。但那个能力一直是
    // "只允许调试、必须两域同值"的状态（两域系数一旦分叉，玩家与刚体的新鲜度就不一致，
    // 业务与物理对"世界时间"的理解也会分叉，见《玩法-减法清单》砍项 13）。
    // 2026-11-24 按"全局统一控制"的决定把分域接口整体删掉：
    // 现在只有 setScale/scale 一对，业务与物理**必然**用同一个系数。
    //
    // 生效对象：业务 tick 与物理 tick 的步长推进；输入/渲染完全不读（墙钟原生）。
    class TimeManager {
        // 无锁原子保证见 core/AtomicGuarantees.hpp
    public:
        // B22：显式禁止拷贝/移动（原来靠 atomic 成员隐式失效）。
        TimeManager() = default;
        TimeManager(const TimeManager&) = delete;
        TimeManager& operator=(const TimeManager&) = delete;

        // 设置整个世界的时间缩放系数。
        // B21：输入校验 —— NaN / 负数 / 0 都会让下游 dt 异常（0 让世界冻结，
        // 负数让时间倒流）。夹到 [0, 100]，NaN 退化为 1.0。
        void setScale(double s) noexcept {
            if (!std::isfinite(s)) s = 1.0;
            if (s < 0.0) s = 0.0;
            if (s > 100.0) s = 100.0;
            scale_.store(s, std::memory_order_relaxed);
        }

        // 当前时间缩放系数（默认 1.0）。业务与物理都读这一个值。
        double scale() const noexcept {
            return scale_.load(std::memory_order_relaxed);
        }

    private:
        std::atomic<double> scale_{ 1.0 };
    };

} // namespace engine::core

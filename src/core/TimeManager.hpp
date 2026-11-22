#pragma once
#include <atomic>
#include <cassert>
#include <cmath>   // B21：std::isfinite

namespace engine::core {

    // 模拟域枚举：业务 / 物理
    enum class Domain { Business, Physics };

    // 全局时间缩放（time_scale）。
    // 最终生效系数 = globalScale（服务端权威，整个世界流速） × 域缩放（默认 1，调试/局部缩放用）。
    // 生效对象：业务与物理两个模拟域；输入线程完全不读（墙钟原生）。
    //
    // ⚠️ 规则（2026-11-17 定）：**正常运行只允许 setGlobalScale，禁止单独调某个域**。
    //    两个域必须用同一个系数一起缩放。原因见 setDomainScale 上方的说明。
    //    → 实现上等价于：business_ 与 physics_ 恒为 1，只有 global_ 在变。
    //    → 玩家可感知的时间控制 = "整个世界一起快慢"（**只有一条时间线**），
    //      不是"某艘船慢下来"（那要按物体分域，已明确不做，见《玩法-减法清单》砍项 13）。
    class TimeManager {
        //检测原子变量double是否无锁
        static_assert(std::atomic<double>::is_always_lock_free,
            "TimeManager requires lock-free atomic<double>; "
            "this platform's atomic<double> is not lock-free");
    public:
        // B22：显式禁止拷贝/移动（原来靠 atomic 成员隐式失效）。
        TimeManager() = default;
        TimeManager(const TimeManager&) = delete;
        TimeManager& operator=(const TimeManager&) = delete;

        // 服务端权威：设置整个世界的时间缩放系数（业务、物理一起缩放）。
        // B21：输入校验 —— NaN / 负数 / 0 都会让下游 dt 异常（0 会让世界冻结，
        // 负数会让时间倒流）。这里夹到合法区间 [0, 100]，NaN 退化为 1.0。
        void setGlobalScale(double s) noexcept {
            if (!std::isfinite(s)) s = 1.0;
            if (s < 0.0) s = 0.0;
            if (s > 100.0) s = 100.0;
            global_.store(s, std::memory_order_relaxed);
        }
        // 读取当前全局时间缩放系数（默认 1.0）。
        double globalScale() const noexcept {
            return global_.load(std::memory_order_relaxed);
        }

        // 单独调整某域的缩放系数（默认 1.0）。
        //
        // ⚠️⚠️ 禁止在正常运行期使用 ⚠️⚠️
        // 业务与物理是两条独立推进的时间线（各自 tick 自己的 sim 时间）。一旦两域系数不同，
        // 它们对"世界时间"的理解就开始分叉，会连出一串难查的 bug：
        //
        //   ① 刚体与玩家"新鲜度"不一致
        //      玩家：物理写 PlayerBodyState → 输入/渲染线程直读 + 插值（平滑）
        //      刚体：物理写回执队列 → 业务线程下一 tick 转发 → 渲染读（无插值）
        //      业务域调慢 → 刚体中继更慢，玩家依旧平滑 → 站上飞船时相对漂移。
        //      业务域调到 0 → 刚体位姿完全停更，玩家还能动 → 玩家穿过飞船。
        //
        //   ② 两个域的世界状态会分叉
        //      业务以为过了 1 秒、物理以为过了 2 秒。现在不炸只是因为业务只做
        //      离散动作（放/拆方块）；将来业务侧一旦有连续计时（方块行为、自动化
        //      机器计时），立刻会算错。
        //
        //   ③ 确定性失效
        //      回放、联机同步都要求"两个域按同一节奏推进"，单独缩放直接破坏这个前提。
        //
        // 唯一允许的用法：调试某一条轨时临时设置，且**必须同时把另一个域设成相同值**。
        // 想整体加速/减速世界 → 用 setGlobalScale()。
        void setDomainScale(Domain d, double s) noexcept {
            domainOf(d).store(s, std::memory_order_relaxed);
        }

        // 某域当前缩放系数（默认 1.0）。
        double domainScale(Domain d) const noexcept {
            return domainOf(d).load(std::memory_order_relaxed);
        }

        // 某域最终生效系数 = globalScale × 域缩放。
        //
        // 注意：global 与域缩放是两次独立 load，不保证一致性快照。
        // 并发写入时，返回值可能是"旧 global × 新 domain"的拼接值，不对应任何
        // 真实时刻——但只影响一帧的 dt，下一 tick 自动恢复。
        // 当前物理用固定步长、时间缩放只控制模拟频率，这个取舍可接受。
        // 若将来做确定性物理（回放/网络同步）或"所有域同帧暂停"，需改为
        // 原子打包（struct ScalePair）或 seqlock，确保读到一致快照。
        double effectiveScale(Domain d) const noexcept {
            return global_.load(std::memory_order_relaxed)
                * domainOf(d).load(std::memory_order_relaxed);
        }

    private:
        const std::atomic<double>& domainOf(Domain d) const noexcept {
            // B20：三元表达式的 else 分支会把**任何**非 Physics 值（含非法枚举）静默
            // 当成 Business。调试期用断言把误传立刻暴露；release 行为保持不变。
            assert(d == Domain::Business || d == Domain::Physics);
            return d == Domain::Physics ? physics_ : business_;
        }
        std::atomic<double>& domainOf(Domain d) noexcept {
            assert(d == Domain::Business || d == Domain::Physics);
            return d == Domain::Physics ? physics_ : business_;
        }

        std::atomic<double> global_{ 1.0 };
        std::atomic<double> business_{ 1.0 };
        std::atomic<double> physics_{ 1.0 };
    };

} // namespace engine::core
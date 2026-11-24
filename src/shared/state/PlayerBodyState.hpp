#pragma once
#include <algorithm>
#include "core/Log.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace engine::shared {

// 玩家体状态（共享槽）：物理线程每 tick 写快照，输入/渲染线程按**本地单调时钟**插值。
//
// 设计照搬 Minecraft 的 Timer 模型（net.minecraft.client.Timer / DeltaTracker.Timer）：
//   物理线程只发布"整数 tick 号 + 时间原点 + 每 tick 周期"；
//   渲染端用**自己的单调时钟**算：
//       renderTick = (now - originNs) / stepNs - kDelayTicks
//       alpha      = frac(renderTick)
//   再取 tick = floor(renderTick) 与 floor+1 两条快照插值。
//
// **为什么不能把"快照到达的墙钟"当时间轴**（这是之前的错误）：
//   物理 tick 的到达间隔实测抖在 30~48ms（累加器驱动）。按到达时刻插值，
//   间隔 30ms 的那段要走完一个 tick 的位移 → 视觉上快 1.3 倍，48ms 则慢；
//   而且渲染端每个 tick 都要重新锚定一次，抖动被逐次注入 → 沿运动方向来回。
//   MC 不需要跨线程传时间，所以没有这个问题。
//
// 延迟 kDelayTicks：渲染进度落后最新 tick 若干个，保证"要插值的那两帧已经收到"。
//   2.5 tick ≈ 100ms（单机沙盒无感），足以吸收 ±10ms 的 tick 到达抖动。
struct PlayerBodyState {
    static constexpr int    kRingSize   = 5;      // 需覆盖 [tick-3, tick] 范围
    static constexpr double kStepSimMs  = 40.0;   // 每 tick 模拟前进 40ms（固定）
    // 渲染延迟（1 tick = 40ms）。物理直接写共享环/快照表，没有中间跳数，
    // 所以玩家与刚体都能用同一个最小延迟（原来刚体走两跳队列时曾需要 2 tick）。
    static constexpr double kDelayTicks = 1.0;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "PlayerBodyState requires lock-free atomic<uint64_t>");

    // 环形缓冲：位置 + 该快照的**整数 tick 号**
    std::atomic<double>       ringX[kRingSize] = {};
    std::atomic<double>       ringY[kRingSize] = {};
    std::atomic<double>       ringZ[kRingSize] = {};
    std::atomic<std::int64_t> ringTick[kRingSize] = {};
    std::atomic<int>          newestIdx{0};
    std::atomic<int>          ringCount{0};

    // 时间基准（物理线程首次写入时定一次，之后不变）
    std::atomic<std::int64_t> originNs{0};        // tick 0 的墙钟时刻
    std::atomic<std::int64_t> stepNs{0};          // 每 tick 的墙钟周期（0.04/scale 秒）
    // 安全重锚计数：物理侧发现渲染进度落后太多（丢过欠账）时 +1，渲染侧据此重建映射
    std::atomic<int>          epoch{0};

    std::atomic<std::uint64_t> seq{0};
    std::atomic<double> yaw{0.0};
    std::atomic<bool>   valid{false};

    // 写入一条快照（物理线程，单写者）。tick 从 1 开始单调递增。
    void push(double x, double y, double z, std::int64_t tick, std::int64_t stepWallNs) {
        const std::uint64_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_release);
        // 时间原点：正常情况**不动**（避免把 tick 到达抖动引进来）；
        // 只有出现"系统性偏差"时才重锚一次 —— 例如启动补跑让 tickCount 比墙钟超前几百 ms，
        // 那种偏差一旦产生会永久存在，渲染端请求的 tick 就永远落在环外（实测超前 10 个 tick
        // → 每帧都走兜底分支 → 冻结几帧再跳）。
        {
            const std::int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const std::int64_t cand = nowNs - stepWallNs * tick;
            const std::int64_t cur = originNs.load(std::memory_order_relaxed);
            if (cur == 0) {
                originNs.store(cand, std::memory_order_relaxed);   // 首条：直接定
            } else {
                // 连续小步校正：每 tick 把 origin 朝 cand 挪 5%。
                // 不用"超阈值就跳"——那样 origin 会突然跳 200ms（=5 个 tick），
                // 渲染端的 pair 跟着跳，抖动幅度反而变大（实测）。
                // 5%/tick ⇒ 启动时的错位 ~10 tick 在 1 秒内平滑收敛，过程中只表现为
                // 极轻微的速度偏差（0.05 tick/tick），看不出来。
                const std::int64_t diff = cand - cur;
                originNs.store(cur + static_cast<std::int64_t>(
                    static_cast<double>(diff) * 0.05), std::memory_order_relaxed);
            }
        }
        const int idx = (newestIdx.load(std::memory_order_relaxed) + 1) % kRingSize;
        ringX[idx].store(x, std::memory_order_relaxed);
        ringY[idx].store(y, std::memory_order_relaxed);
        ringZ[idx].store(z, std::memory_order_relaxed);
        ringTick[idx].store(tick, std::memory_order_relaxed);
        newestIdx.store(idx, std::memory_order_relaxed);
        const int cnt = ringCount.load(std::memory_order_relaxed);
        if (cnt < kRingSize) ringCount.store(cnt + 1, std::memory_order_relaxed);
        stepNs.store(stepWallNs, std::memory_order_relaxed);
        seq.store(s + 2, std::memory_order_release);
        valid.store(true, std::memory_order_relaxed);
    }

    // 渲染端：请求"重锚"（物理丢过欠账 / 时钟漂移过大时）。
    void requestReanchor() { epoch.fetch_add(1, std::memory_order_relaxed); }

    // 当前渲染进度的小数部分（0~1）：tick N → N+1 之间走了多少。
    // 刚体等**非玩家**对象也用它插值 —— 它们和玩家在同一条 25Hz 时间线上，
    // 用同一个 alpha 才能保证"玩家站在飞船上不漂移"。
    // 当前渲染时刻在**模拟时间轴**上的位置（秒）。刚体等非玩家对象用它做时间轴索引，
    // 与玩家同一域 —— 这样"玩家站在飞船上"两者相位严格一致。
    double renderSimSeconds() const { return renderTickF() * (kStepSimMs / 1000.0); }
    // 当前渲染时刻的浮点 tick 号（相对 tickCount 的域）。玩家与刚体共用。
    double renderTickF() const {
        const std::int64_t origin = originNs.load(std::memory_order_relaxed);
        const std::int64_t step   = stepNs.load(std::memory_order_relaxed);
        if (origin <= 0 || step <= 0) return 0.0;
        const std::int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return static_cast<double>(nowNs - origin) / static_cast<double>(step) - kDelayTicks;
    }

    // 插值读取（输入/渲染线程）：纯本地单调时钟，不依赖快照到达时刻。
    void readInterpolated(double& ox, double& oy, double& oz) const {
        if (!valid.load(std::memory_order_relaxed)) {
            ox = 0.0; oy = 0.0; oz = 0.0;
            return;
        }
        const std::int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        double  xs[kRingSize], ys[kRingSize], zs[kRingSize];
        std::int64_t ticks[kRingSize];
        int newest = 0, count = 0;
        std::int64_t origin = 0, step = 0;

        bool ok = false;
        for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
            const std::uint64_t s0 = seq.load(std::memory_order_acquire);
            if ((s0 & 1ULL) != 0ULL) continue;
            for (int i = 0; i < kRingSize; ++i) {
                xs[i] = ringX[i].load(std::memory_order_relaxed);
                ys[i] = ringY[i].load(std::memory_order_relaxed);
                zs[i] = ringZ[i].load(std::memory_order_relaxed);
                ticks[i] = ringTick[i].load(std::memory_order_relaxed);
            }
            newest  = newestIdx.load(std::memory_order_relaxed);
            count   = ringCount.load(std::memory_order_relaxed);
            origin  = originNs.load(std::memory_order_relaxed);
            step    = stepNs.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s0 == seq.load(std::memory_order_acquire)) ok = true;
        }
        if (!ok) {
            ox = ringX[newest].load(std::memory_order_relaxed);
            oy = ringY[newest].load(std::memory_order_relaxed);
            oz = ringZ[newest].load(std::memory_order_relaxed);
            return;
        }
        if (origin <= 0 || step <= 0 || count <= 0) {
            ox = xs[newest]; oy = ys[newest]; oz = zs[newest];
            return;
        }

        // ---- MC 同款：本地单调时钟 → 浮点 tick 号 ----
        const double tickNow = static_cast<double>(nowNs - origin) / static_cast<double>(step);
        const double renderTick = tickNow - kDelayTicks;
        const std::int64_t baseTick = static_cast<std::int64_t>(std::floor(renderTick));
        const double alpha = std::clamp(renderTick - static_cast<double>(baseTick), 0.0, 1.0);

        // ---- 在环里找 tick == baseTick 与 baseTick+1 ----
        int i0 = -1, i1 = -1;
        for (int k = 0; k < count; ++k) {
            const int i = (newest - k + kRingSize) % kRingSize;
            if (ticks[i] == baseTick) i0 = i;
            if (ticks[i] == baseTick + 1) i1 = i;
        }
        if (i0 >= 0 && i1 >= 0) {
            ox = xs[i0] + (xs[i1] - xs[i0]) * alpha;
            oy = ys[i0] + (ys[i1] - ys[i0]) * alpha;
            oz = zs[i0] + (zs[i1] - zs[i0]) * alpha;
            return;
        }
        // ===== 临时诊断（测完删）：兜底命中计数（1Hz）=====
        {
            static std::atomic<long long> miss{0}, total{0};
            static thread_local long long lastLogNs = 0;
            total.fetch_add(1, std::memory_order_relaxed);
            miss.fetch_add(1, std::memory_order_relaxed);
            const std::int64_t now2 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (lastLogNs == 0) lastLogNs = now2;
            if (now2 - lastLogNs >= 1000000000LL) {
                const long long m = miss.exchange(0);
                const long long n = total.exchange(0);
                ENGINE_LOG_INFO("Render",
                    "[兜底] 每秒 " + std::to_string(m) + " / " + std::to_string(n) + " 帧没找到精确帧对");
                lastLogNs = now2;
            }
        }
        // 环里没有正好那两帧（首帧 / 刚重锚）：夹到环内最近的 tick 再插值
        int lo = -1, hi = -1;
        for (int k = 0; k < count; ++k) {
            const int i = (newest - k + kRingSize) % kRingSize;
            if (ticks[i] <= baseTick && (lo < 0 || ticks[i] > ticks[lo])) lo = i;
            if (ticks[i] >= baseTick + 1 && (hi < 0 || ticks[i] < ticks[hi])) hi = i;
        }
        if (lo >= 0 && hi >= 0 && ticks[hi] > ticks[lo]) {
            const double a = static_cast<double>(baseTick - ticks[lo]) /
                             static_cast<double>(ticks[hi] - ticks[lo]);
            ox = xs[lo] + (xs[hi] - xs[lo]) * a;
            oy = ys[lo] + (ys[hi] - ys[lo]) * a;
            oz = zs[lo] + (zs[hi] - zs[lo]) * a;
            return;
        }
        ox = xs[newest]; oy = ys[newest]; oz = zs[newest];
    }
};

} // namespace engine::shared

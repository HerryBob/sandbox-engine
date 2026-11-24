#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace engine::shared {

// 玩家体状态（共享槽）：物理线程每 tick 写，输入线程与渲染线程插值读。
// 位置语义与 PhysicsBody 一致：脚底中心，眼高偏移由读取方自行叠加。
//
// 为什么需要插值：物理是固定步长（dt = 0.04 秒，25Hz），而渲染约 165Hz。
//
// **关键：插值参数必须用「模拟时间」，不能用「快照到达的墙钟时刻」。**
//   物理每 tick 恒定前进 0.04s，但 tick 到达墙钟的间隔会抖（实测 26~52ms）。
//   若按到达时刻插值：间隔 26ms 的那一段走完同样一个 tick 的位移 → 视觉上走快了 1.5 倍，
//   间隔 52ms 的那一段则走慢 —— 这就是"持续小抖动"的真正来源。
//   改成按模拟时间插值后：渲染端用**自己的时钟**（每帧 += 帧dt，按 stepSim/stepWall 换算），
//   在两份快照的模拟时刻之间取 alpha。tick 到达早晚只影响"追赶"，不影响画面速度。
//
// 三槽环 + 延迟 1.5 tick：渲染时刻恒落后最新快照 1.5 个 tick，
//   保证它永远落在"两份旧快照之间"，alpha 不触顶（触顶会产生"短帧"）。
//   代价：固定 ~60ms 视觉延迟（单机沙盒无感）。这是 MC partialTicks 的同一思路。
struct PlayerBodyState {
    static constexpr int kRingSize = 3;
    static constexpr double kStepSimMs = 40.0;   // 每 tick 模拟前进 40ms（固定，不随 scale 变）
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "PlayerBodyState requires lock-free atomic<uint64_t>");

    // 环形缓冲：位置 + 该快照对应的**模拟时刻**（毫秒）
    std::atomic<double>      ringX[kRingSize] = {};
    std::atomic<double>      ringY[kRingSize] = {};
    std::atomic<double>      ringZ[kRingSize] = {};
    std::atomic<double>      ringSimMs[kRingSize] = {};
    std::atomic<int>         newestIdx{0};
    std::atomic<int>         ringCount{0};
    // 该 tick 的**墙钟**周期（纳秒）：只用来把"帧 dt"换算成"模拟时间增量"。
    // scale≠1 时它 ≠ 40ms（周期 = 0.04/scale）。
    std::atomic<std::int64_t> stepWallNs{0};
    std::atomic<std::uint64_t> seq{0};

    // 朝向（弧度，累加不清零，yaw = 0 时前方向朝 -Z，与相机 yaw 同一约定）。
    // 读侧直接取最新值、不插值（鼠标本身就是每帧连续更新的）。
    std::atomic<double> yaw{0.0};
    std::atomic<bool>   valid{false};

    // 写入一条快照（物理线程，单写者）。
    // simMs：本 tick 结束时的模拟时刻（毫秒）；wallNs：本 tick 的墙钟周期（纳秒）。
    void push(double x, double y, double z, double simMs, std::int64_t wallNs) {
        const std::uint64_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_release);
        const int idx = (newestIdx.load(std::memory_order_relaxed) + 1) % kRingSize;
        ringX[idx].store(x, std::memory_order_relaxed);
        ringY[idx].store(y, std::memory_order_relaxed);
        ringZ[idx].store(z, std::memory_order_relaxed);
        ringSimMs[idx].store(simMs, std::memory_order_relaxed);
        newestIdx.store(idx, std::memory_order_relaxed);
        const int cnt = ringCount.load(std::memory_order_relaxed);
        if (cnt < kRingSize) ringCount.store(cnt + 1, std::memory_order_relaxed);
        stepWallNs.store(wallNs, std::memory_order_relaxed);
        seq.store(s + 2, std::memory_order_release);
        valid.store(true, std::memory_order_relaxed);
    }

    // 插值读取：渲染时刻 = 本线程时钟（模拟时间域），落后最新快照 1.5 tick。
    void readInterpolated(double& ox, double& oy, double& oz) const {
        if (!valid.load(std::memory_order_relaxed)) {
            ox = 0.0; oy = 0.0; oz = 0.0;
            return;
        }
        const std::int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        double  xs[kRingSize], ys[kRingSize], zs[kRingSize], sims[kRingSize];
        int newest = 0, count = 0;
        std::int64_t wallStep = 0;

        // ---- 环形数据（seqlock 成组读）----
        bool ok = false;
        for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
            const std::uint64_t s0 = seq.load(std::memory_order_acquire);
            if ((s0 & 1ULL) != 0ULL) continue;
            for (int i = 0; i < kRingSize; ++i) {
                xs[i] = ringX[i].load(std::memory_order_relaxed);
                ys[i] = ringY[i].load(std::memory_order_relaxed);
                zs[i] = ringZ[i].load(std::memory_order_relaxed);
                sims[i] = ringSimMs[i].load(std::memory_order_relaxed);
            }
            newest = newestIdx.load(std::memory_order_relaxed);
            count  = ringCount.load(std::memory_order_relaxed);
            wallStep = stepWallNs.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s0 == seq.load(std::memory_order_acquire)) ok = true;
        }
        if (!ok) {   // 连续撞写窗口：退回最新值（宁可抖一下，不给错位置）
            ox = ringX[newest].load(std::memory_order_relaxed);
            oy = ringY[newest].load(std::memory_order_relaxed);
            oz = ringZ[newest].load(std::memory_order_relaxed);
            return;
        }

        // ---- 本线程的渲染时钟（模拟时间域，毫秒）----
        // 每帧按"帧 dt × (stepSim/stepWall)"推进 —— 与 tick 到达时刻无关。
        static thread_local double renderSimMs = -1.0;
        static thread_local std::int64_t lastCallNs = 0;
        const double newestSim = sims[newest];
        const double wallStepMs = (wallStep > 0) ? wallStep / 1e6 : kStepSimMs;
        const double rate = (wallStepMs > 0.01) ? (kStepSimMs / wallStepMs) : 1.0;
        if (renderSimMs < 0.0) {
            renderSimMs = newestSim - 1.5 * kStepSimMs;      // 首次：直接落后 1.5 tick
        } else if (lastCallNs > 0) {
            const double dtWallMs = (nowNs - lastCallNs) / 1e6;
            renderSimMs += dtWallMs * rate;
        }
        lastCallNs = nowNs;

        // ---- 夹紧：落后太多就对齐；领先就压住（不外推）----
        if (renderSimMs < newestSim - 1.9 * kStepSimMs) {
            renderSimMs = newestSim - 1.5 * kStepSimMs;
        }
        if (renderSimMs > newestSim - 0.2 * kStepSimMs) {
            renderSimMs = newestSim - 0.2 * kStepSimMs;
        }

        // ---- 在环里找夹住 renderSimMs 的相邻两帧（按模拟时刻）----
        for (int k = 0; k + 1 < count; ++k) {
            const int i = (newest - k + kRingSize) % kRingSize;          // 新 → 旧
            const int j = (newest - k - 1 + kRingSize) % kRingSize;      // 更旧
            if (sims[j] <= renderSimMs && renderSimMs <= sims[i]) {
                const double span = sims[i] - sims[j];
                double alpha = (span > 1e-9) ? (renderSimMs - sims[j]) / span : 1.0;
                alpha = std::clamp(alpha, 0.0, 1.0);
                ox = xs[j] + (xs[i] - xs[j]) * alpha;
                oy = ys[j] + (ys[i] - ys[j]) * alpha;
                oz = zs[j] + (zs[i] - zs[j]) * alpha;
                return;
            }
        }
        // 兜底：渲染时刻落在环外（首帧/长暂停）→ 用最旧与最新之间插值
        const int oldest = (newest - count + 1 + kRingSize) % kRingSize;
        ox = xs[oldest]; oy = ys[oldest]; oz = zs[oldest];
    }
};

} // namespace engine::shared

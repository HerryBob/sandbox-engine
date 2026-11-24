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
// 直接读最新位置会得到"每 40ms 跳一次"的阶梯信号，视觉上是 judder。
//
// **为什么是三快照环 + 延迟 1.5 tick**（而不是只存 prev/curr）：
//   165fps ÷ 25Hz = 6.6 帧/tick —— 每个 tick 之间是"6 帧 + 1 个短帧"交替。
//   只存两份时 alpha 会爬到 1 然后被钳住等下一个 tick，那一帧位移变小，
//   每约 3 个 tick 重复一次 → 持续的小抖动（实测瞬时速度在 0 与 2.5x 之间跳）。
//   改成存三份、渲染时刻取 `now - 1.5*tick`：渲染时刻永远落在"两份旧快照之间"，
//   alpha 不再触顶，每帧位移严格正比于帧间隔。
//   代价：固定 ~60ms 视觉延迟（单机沙盒无感）。这就是 MC 的 partialTicks 思路，
//   只是 MC 落后 1 tick、这里落后 1.5 tick（多出的 0.5 吸收"物理偶尔晚到"）。
struct PlayerBodyState {
    static constexpr int kRingSize = 3;
    // B45：用定宽类型（std::uint64_t / std::int64_t）而不是 unsigned long long /
    // long long —— 后者在 LP64 与 LLP64 上宽度不同，跨平台时 seq 的溢出行为会变。
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "PlayerBodyState requires lock-free atomic<uint64_t>");

    // 环形缓冲的三份快照（seqlock 保护整环：位置与到达时刻必须成组读）
    std::atomic<double>      ringX[kRingSize] = {};
    std::atomic<double>      ringY[kRingSize] = {};
    std::atomic<double>      ringZ[kRingSize] = {};
    std::atomic<std::int64_t> ringNs[kRingSize] = {};   // 该条快照的到达墙钟（纳秒）
    std::atomic<int>         newestIdx{0};              // 最新一条的下标
    std::atomic<int>         ringCount{0};              // 已写入条数（<3 时按实际数量插值）
    std::atomic<std::int64_t> dtNs{0};                  // 标称步长（纳秒），0 = 尚未写入
    std::atomic<std::uint64_t> seq{0};                  // 奇数 = 写入中

    // 朝向（弧度，累加不清零，yaw = 0 时前方向朝 -Z，与相机 yaw 同一约定）。
    // 写者：输入线程。读侧直接取最新值、不插值（鼠标本身就是每帧连续更新的）。
    std::atomic<double> yaw{0.0};
    std::atomic<bool>   valid{false};

    // 写入一条快照（物理线程，单写者）。
    void push(double x, double y, double z, std::int64_t nowNs, std::int64_t stepNs) {
        const std::uint64_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_release);        // 奇数 = 写入中
        const int idx = (newestIdx.load(std::memory_order_relaxed) + 1) % kRingSize;
        ringX[idx].store(x, std::memory_order_relaxed);
        ringY[idx].store(y, std::memory_order_relaxed);
        ringZ[idx].store(z, std::memory_order_relaxed);
        ringNs[idx].store(nowNs, std::memory_order_relaxed);
        newestIdx.store(idx, std::memory_order_relaxed);
        const int cnt = ringCount.load(std::memory_order_relaxed);
        if (cnt < kRingSize) ringCount.store(cnt + 1, std::memory_order_relaxed);
        dtNs.store(stepNs, std::memory_order_relaxed);
        seq.store(s + 2, std::memory_order_release);        // 偶数 = 稳定
        valid.store(true, std::memory_order_relaxed);
    }

    // 插值读取（输入/渲染线程）：渲染时刻 = now - 1.5 * 标称步长。
    // 在环里找"夹住渲染时刻"的相邻两帧做线性插值。
    void readInterpolated(double& ox, double& oy, double& oz) const {
        if (!valid.load(std::memory_order_relaxed)) {
            ox = 0.0; oy = 0.0; oz = 0.0;
            return;
        }
        const std::int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // 局部副本：一次 seqlock 读把整环取出来
        double  xs[kRingSize], ys[kRingSize], zs[kRingSize];
        std::int64_t ts[kRingSize];
        int newest = 0, count = 0;
        std::int64_t step = 0;

        for (int attempt = 0; attempt < 4; ++attempt) {
            const std::uint64_t s0 = seq.load(std::memory_order_acquire);
            if ((s0 & 1ULL) != 0ULL) continue;
            for (int i = 0; i < kRingSize; ++i) {
                xs[i] = ringX[i].load(std::memory_order_relaxed);
                ys[i] = ringY[i].load(std::memory_order_relaxed);
                zs[i] = ringZ[i].load(std::memory_order_relaxed);
                ts[i] = ringNs[i].load(std::memory_order_relaxed);
            }
            newest = newestIdx.load(std::memory_order_relaxed);
            count  = ringCount.load(std::memory_order_relaxed);
            step   = dtNs.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s0 != seq.load(std::memory_order_acquire)) continue;

            // 渲染时刻：落后 1.5 个标称步长（step 未就绪时按 40ms 兜底）
            const std::int64_t delay = (step > 0 ? step : 40000000LL) * 3 / 2;
            const std::int64_t renderNs = nowNs - delay;

            // 按时间从旧到新走一遍，找夹住 renderNs 的相邻两帧
            int prevI = -1, nextI = -1;
            for (int k = 0; k < count; ++k) {
                const int i = (newest - k + kRingSize) % kRingSize;   // newest 往回数
                const int j = (newest - k - 1 + kRingSize) % kRingSize;
                if (k + 1 >= count) { prevI = j; nextI = i; break; }  // 只剩两帧时兜底
                if (ts[j] <= renderNs && renderNs <= ts[i]) { prevI = j; nextI = i; break; }
            }
            if (prevI < 0) {   // 渲染时刻比最旧还旧，或比最新还新：贴住端点
                const int oldest = (newest - count + 1 + kRingSize) % kRingSize;
                if (renderNs < ts[oldest]) { ox = xs[oldest]; oy = ys[oldest]; oz = zs[oldest]; }
                else { ox = xs[newest]; oy = ys[newest]; oz = zs[newest]; }
                return;
            }
            const std::int64_t span = ts[nextI] - ts[prevI];
            double alpha = 0.0;
            if (span > 0) {
                alpha = static_cast<double>(renderNs - ts[prevI]) / static_cast<double>(span);
            }
            alpha = std::clamp(alpha, 0.0, 1.0);
            ox = xs[prevI] + (xs[nextI] - xs[prevI]) * alpha;
            oy = ys[prevI] + (ys[nextI] - ys[prevI]) * alpha;
            oz = zs[prevI] + (zs[nextI] - zs[prevI]) * alpha;
            return;
        }

        // 连续四次撞上写窗口：退回最新值（宁可抖一下，也不给错位置）
        ox = ringX[newest].load(std::memory_order_relaxed);
        oy = ringY[newest].load(std::memory_order_relaxed);
        oz = ringZ[newest].load(std::memory_order_relaxed);
    }
};

} // namespace engine::shared

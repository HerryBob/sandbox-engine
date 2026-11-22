#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace engine::shared {

// 玩家体状态（共享槽）：物理线程每 tick 写，输入线程与渲染线程插值读。
// 位置语义与 RigidBody 一致：脚底中心，眼高偏移由读取方自行叠加。
//
// 为什么需要插值：物理是固定步长（dt = 0.04 秒，25Hz），而渲染约 165Hz、输入约 125Hz。
// 直接读最新位置会得到"每 40ms 跳一次"的阶梯信号，视觉上是 judder（步进抖动），
// 帧率越高反而越明显。这里同时保存本 tick 的起点（prev*）与终点（x/y/z）位置，
// 读侧按 alpha = (现在 - tick 开始时刻) / tick 时长 在两份快照之间混合，
// 得到任意帧率下都连续的位置。
//
// 八个值必须成组读：3 个 prev + 3 个 curr + tickStartNs + tickDurationNs。
// 读到"半新半旧"的一对位置会表现为瞬间的位置跳变；而起点/时长若和位置不配对，
// alpha 会算错（同样是一帧的位置跳变）。
// 因此用 seqlock（序列锁）包住——版本号奇数表示写者正在写，偶数且读前后一致才采信。
struct PlayerBodyState {
    // ---- 插值快照（seqlock 保护）----
    // B45：用定宽类型（std::uint64_t / std::int64_t）而不是 unsigned long long /
    // long long —— 后者在 LP64 与 LLP64 上宽度不同，跨平台时 seq 的溢出行为会变。
    // B46：这个槽在物理线程 25Hz 写、输入/渲染线程高频读，必须真无锁
    // （非 lock-free 平台上 atomic<uint64_t> 会退化成锁）。
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "PlayerBodyState requires lock-free atomic<uint64_t>");
    std::atomic<std::uint64_t> seq{ 0 };
    std::atomic<double> prevX{ 0.0 };
    std::atomic<double> prevY{ 0.0 };
    std::atomic<double> prevZ{ 0.0 };
    std::atomic<double> x{ 0.0 };
    std::atomic<double> y{ 0.0 };
    std::atomic<double> z{ 0.0 };
    // 本 tick 的墙钟起点（steady_clock 纳秒计数）与时长（纳秒）。
    // 读侧用它算插值进度，两个线程用同一个 steady_clock，不涉及跨时基换算。
    // 时长不是常量：写者每 tick 用"上次写入到这次的墙钟差"更新（自适应 time_scale 与
    // 调度抖动），必须与 tickStartNs 一起、在同一个 seqlock 窗口内更新。
    std::atomic<std::int64_t> tickStartNs{ 0 };
    std::atomic<std::int64_t> tickDurationNs{ 0 };   // 0 = 尚未写入（配合读侧的 durNs > 0 守卫）

    // 朝向（弧度，累加不清零，yaw = 0 时前方向朝 -Z，与相机 yaw 同一约定）。
    // 写者：输入线程（玩家模式下每帧把鼠标累积的 yaw 同步过来）；物理线程不碰它——
    // 朝向由鼠标驱动，不是物理量。
    // 读取方直接取最新值、不做插值：相机 yaw 本身就是每帧连续更新的（125Hz 鼠标事件），
    // 不存在位置那种"25Hz 阶梯"，插值反而会引入一帧延迟。
    // 注意模型侧约定与这里不同（模型 yawDeg = 0 时脸朝 +Z），换算在读取处完成。
    std::atomic<double> yaw{ 0.0 };
    std::atomic<bool>   valid{ false }; // 物理线程尚未写过位置时为 false

    // 插值读取：把 25Hz 的物理位置混合成当帧应该画的位置。
    // ox/oy/oz 输出混合结果；快照不可信（重试耗尽）时退回最新终点值，即退化为不插值。
    void readInterpolated(double& ox, double& oy, double& oz) const {
        // B44：函数自身不设防 —— 物理线程还没写过位置时 valid == false，
        // 此时八个值全是初值 0，插值出来就是"原点"。调用方本该先查 valid，
        // 但漏查的代价是玩家瞬移到 (0,0,0)，所以这里直接给出"未就绪 → 全 0"，
        // 与终点值一致（x/y/z 初值也是 0），不会产生额外的假动作。
        if (!valid.load(std::memory_order_relaxed)) {
            ox = 0.0; oy = 0.0; oz = 0.0;
            return;
        }

        const std::int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        for (int attempt = 0; attempt < 4; ++attempt) {
            const std::uint64_t s0 = seq.load(std::memory_order_acquire);
            if ((s0 & 1ULL) != 0ULL) continue; // 写者正在写，重试

            const double pxs = prevX.load(std::memory_order_relaxed);
            const double pys = prevY.load(std::memory_order_relaxed);
            const double pzs = prevZ.load(std::memory_order_relaxed);
            const double cxs = x.load(std::memory_order_relaxed);
            const double cys = y.load(std::memory_order_relaxed);
            const double czs = z.load(std::memory_order_relaxed);
            const std::int64_t startNs = tickStartNs.load(std::memory_order_relaxed);
            const std::int64_t durNs   = tickDurationNs.load(std::memory_order_relaxed);

            // B43：数据读完之后必须插一道 acquire fence。
            // 否则在弱内存序平台（ARM64）上，上面这些 relaxed 读可能被重排到
            // 下面第二次 seq 读之后 —— 结果是"版本号前后一致，但数据半新半旧"，
            // 正是 seqlock 要防的那种撕裂。x86 只是侥幸（load-load 不重排）。
            std::atomic_thread_fence(std::memory_order_acquire);

            // 读完再验一次版本号：一致说明这组八个值来自同一个 tick。
            const std::uint64_t s1 = seq.load(std::memory_order_acquire);
            if (s0 != s1) continue;

            double alpha = 0.0;
            if (durNs > 0 && startNs > 0) {
                alpha = static_cast<double>(nowNs - startNs) /
                        static_cast<double>(durNs);
            }
            alpha = std::clamp(alpha, 0.0, 1.0); // 物理落后时停在终点，不外推

            ox = pxs + (cxs - pxs) * alpha;
            oy = pys + (cys - pys) * alpha;
            oz = pzs + (czs - pzs) * alpha;
            return;
        }

        // 连续四次都撞上写窗口（极端情况）：退回终点值，宁可抖一下也不给错位置。
        ox = x.load(std::memory_order_relaxed);
        oy = y.load(std::memory_order_relaxed);
        oz = z.load(std::memory_order_relaxed);
    }
};

} // namespace engine::shared

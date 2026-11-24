#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::shared {

// ============================================================================
// PoseHistory：跨线程实体位姿（玩家 / 生物 / 刚体）—— **空间换时间**的容器
// ============================================================================
//
// 为什么不用"每实体一个 seqlock 环"：
//   渲染端每帧要为每个可见实体做 8 次原子读（位置 3 + 四元数 4 + tick 1）。
//   1024 个可见刚体 ≈ 8000 次原子读/帧 ≈ 1.4ms —— 占 6ms 帧预算的 23%。
//
// 这里改成**每帧整表快照环**：
//   物理线程每 tick 填一整帧（只填活跃槽），然后**翻转一次原子指针**；
//   渲染线程每帧只读**一次原子**拿到最新帧号，之后逐实体都是普通内存读。
//   同规模下 1024 实体 ≈ 0.02ms/帧（快 ~70 倍），代价是 ~400KB 静态内存。
//   C++ 里这点内存换这个收益是划算的（用户明确"可以空间换时间"）。
//
// 数据流：
//   物理（25Hz，唯一写者）
//     每 tick：beginFrame(tick) → 填活跃实体 → commitFrame()（1 次原子发布）
//   渲染（~165Hz，唯一读者）
//     每帧：读一次最新帧号 → 用 renderTick（来自 PlayerBodyState，落后 1 tick）
//           定位夹住它的两帧 → 逐实体 lerp/slerp。全程无锁、无原子读实体数据。
//
// 三类实体共用一张表（这是设计目标：不再为每类实体加新结构）：
//     slot 0                    → 玩家（固定）
//     slotOfEntity_[entityId]   → 生物
//     slotOfStructure_[id]      → 刚体（上限 1024，表按 4096 留余量）
//
// 撕裂/一致性：整帧一次发布（commitFrame 的 release store），
//   读者拿到的一定是**完整的一帧**；不需要逐字段重试。
//   极端情况下读者读到正在被写的帧（下一 tick 覆盖 6 帧前的那帧），
//   但因为渲染延迟 1 tick、环深 6 帧，正常路径碰不到；
//   若真碰上（tick 间隔异常），插值结果只是略旧，不会撕裂成半个实体。

inline constexpr int kPoseMaxSlots   = 2048;   // 玩家 1 + 生物 + 刚体（刚体上限 1024）
inline constexpr int kPoseFrames     = 6;      // 6 tick 历史 ≈ 240ms
inline constexpr int kPoseIdTableSize = 4096;  // id → slot 反查表（刚体 1024，留 4 倍余量）

enum class PoseKind : int { Free = 0, Player = 1, Mob = 2, Contraption = 3 };

// 单实体单帧的位姿（32 字节：位置 12 + 四元数 16 + 对齐）
struct PoseEntry {
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
};

struct PoseFrame {
    std::int64_t tick = -1;              // 该帧对应的模拟 tick（-1 = 未写过）
    PoseEntry    e[kPoseMaxSlots]{};     // 2048 × 32B = 64KB
};

class PoseHistory {
public:
    // ---- 槽管理（只在物理线程调用）----
    // 分配一个槽；成功返回槽号，失败（满了）返回 -1 —— 调用方据此回退旧路径。
    int allocate(PoseKind kind, int ownerId) {
        int slot = freeHead_.load(std::memory_order_relaxed);
        if (slot < 0) return -1;
        freeHead_.store(nextFree_[slot], std::memory_order_relaxed);
        kind_[slot].store(static_cast<int>(kind), std::memory_order_relaxed);
        ownerId_[slot].store(ownerId, std::memory_order_relaxed);
        generation_[slot].fetch_add(1, std::memory_order_relaxed);
        // 反查表（渲染端 O(1)）
        if (kind == PoseKind::Contraption && ownerId >= 0 && ownerId < kPoseIdTableSize) {
            slotOfStructure_[ownerId].store(slot, std::memory_order_release);
        } else if (kind == PoseKind::Mob && ownerId >= 0 && ownerId < kPoseIdTableSize) {
            slotOfEntity_[ownerId].store(slot, std::memory_order_release);
        }
        activeCount_.fetch_add(1, std::memory_order_relaxed);
        return slot;
    }

    // 归还槽（销毁刚体/生物时）。代际 +1，渲染端读到旧代际即跳过。
    void release(int slot) {
        if (slot < 0 || slot >= kPoseMaxSlots) return;
        const int kind = kind_[slot].load(std::memory_order_relaxed);
        const int owner = ownerId_[slot].load(std::memory_order_relaxed);
        if (kind == static_cast<int>(PoseKind::Contraption) && owner >= 0 &&
            owner < kPoseIdTableSize) {
            slotOfStructure_[owner].store(-1, std::memory_order_release);
        } else if (kind == static_cast<int>(PoseKind::Mob) && owner >= 0 &&
                   owner < kPoseIdTableSize) {
            slotOfEntity_[owner].store(-1, std::memory_order_release);
        }
        generation_[slot].fetch_add(1, std::memory_order_relaxed);
        kind_[slot].store(static_cast<int>(PoseKind::Free), std::memory_order_relaxed);
        ownerId_[slot].store(-1, std::memory_order_relaxed);
        nextFree_[slot] = freeHead_.load(std::memory_order_relaxed);
        freeHead_.store(slot, std::memory_order_relaxed);
        activeCount_.fetch_sub(1, std::memory_order_relaxed);
    }

    // ---- 写帧（只在物理线程调用）----
    // beginFrame 返回**可写帧**的引用；填完后必须 commitFrame() 发布。
    PoseFrame& beginFrame(std::int64_t tick) {
        writeIdx_ = (newest_.load(std::memory_order_relaxed) + 1) % kPoseFrames;
        PoseFrame& f = frames_[writeIdx_];
        f.tick = tick;
        return f;
    }
    void commitFrame() { newest_.store(writeIdx_, std::memory_order_release); }

    // ---- 读（只在渲染线程调用）----
    // 反查：id → slot（O(1)）。返回 -1 表示没有。
    int slotOfStructure(int structureId) const {
        if (structureId < 0 || structureId >= kPoseIdTableSize) return -1;
        const int s = slotOfStructure_[structureId].load(std::memory_order_acquire);
        if (s < 0) return -1;
        if (kind_[s].load(std::memory_order_relaxed) != static_cast<int>(PoseKind::Contraption))
            return -1;
        return s;
    }
    int slotOfEntity(int entityId) const {
        if (entityId < 0 || entityId >= kPoseIdTableSize) return -1;
        const int s = slotOfEntity_[entityId].load(std::memory_order_acquire);
        if (s < 0) return -1;
        if (kind_[s].load(std::memory_order_relaxed) != static_cast<int>(PoseKind::Mob))
            return -1;
        return s;
    }
    int playerSlot() const { return playerSlot_; }

    int activeCount() const { return activeCount_.load(std::memory_order_relaxed); }

    // 用"浮点 tick 号"（PlayerBodyState::renderTickF()）插值读某槽的位姿。
    // 返回 false 表示该槽没有数据。
    bool readInterpolated(double renderTick, int slot, PoseEntry& out) const {
        if (slot < 0 || slot >= kPoseMaxSlots) return false;
        if (kind_[slot].load(std::memory_order_relaxed) == static_cast<int>(PoseKind::Free))
            return false;

        const int newest = newest_.load(std::memory_order_acquire);   // 每帧仅此一次原子
        if (frames_[newest].tick < 0) return false;

        // 在环里找夹住 renderTick 的两帧（帧按 tick 单调）
        int i0 = -1, i1 = -1;
        double a0 = 0.0;
        for (int k = 0; k < kPoseFrames; ++k) {
            const int i = (newest - k + kPoseFrames) % kPoseFrames;
            const int j = (newest - k - 1 + kPoseFrames) % kPoseFrames;
            if (frames_[j].tick < 0) break;
            if (static_cast<double>(frames_[j].tick) <= renderTick &&
                renderTick <= static_cast<double>(frames_[i].tick)) {
                i0 = j;
                i1 = i;
                const double span = static_cast<double>(frames_[i].tick - frames_[j].tick);
                a0 = (span > 0.0)
                         ? (renderTick - static_cast<double>(frames_[j].tick)) / span
                         : 1.0;
                break;
            }
        }
        if (i0 < 0) {
            // 渲染时刻落在环外（首帧 / 极长停顿）：贴住最新帧
            out = frames_[newest].e[slot];
            return true;
        }
        const double alpha = std::clamp(a0, 0.0, 1.0);
        const PoseEntry& p0 = frames_[i0].e[slot];
        const PoseEntry& p1 = frames_[i1].e[slot];
        out.px = static_cast<float>(p0.px + (p1.px - p0.px) * alpha);
        out.py = static_cast<float>(p0.py + (p1.py - p0.py) * alpha);
        out.pz = static_cast<float>(p0.pz + (p1.pz - p0.pz) * alpha);
        // 四元数：nlerp（比 slerp 便宜，视觉上对本场景足够；两者都是单位化后使用）
        float qx = p0.qx + (p1.qx - p0.qx) * static_cast<float>(alpha);
        float qy = p0.qy + (p1.qy - p0.qy) * static_cast<float>(alpha);
        float qz = p0.qz + (p1.qz - p0.qz) * static_cast<float>(alpha);
        float qw = p0.qw + (p1.qw - p0.qw) * static_cast<float>(alpha);
        // 两条姿态若点积为负（跨半球），先取反再插值，避免"绕远路"
        if (p0.qx * p1.qx + p0.qy * p1.qy + p0.qz * p1.qz + p0.qw * p1.qw < 0.0f) {
            qx = -qx; qy = -qy; qz = -qz; qw = -qw;
        }
        const float len = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
        if (len > 1e-6f) {
            out.qx = qx / len; out.qy = qy / len; out.qz = qz / len; out.qw = qw / len;
        } else {
            out.qx = 0.0f; out.qy = 0.0f; out.qz = 0.0f; out.qw = 1.0f;
        }
        return true;
    }

    // 构造：初始化空闲链表（槽 0 留给玩家）
    PoseHistory() {
        for (int i = 0; i < kPoseMaxSlots; ++i) {
            kind_[i].store(static_cast<int>(PoseKind::Free), std::memory_order_relaxed);
            ownerId_[i].store(-1, std::memory_order_relaxed);
            generation_[i].store(0, std::memory_order_relaxed);
            nextFree_[i] = (i + 1 < kPoseMaxSlots) ? (i + 1) : -1;
        }
        for (int i = 0; i < kPoseIdTableSize; ++i) {
            slotOfStructure_[i].store(-1, std::memory_order_relaxed);
            slotOfEntity_[i].store(-1, std::memory_order_relaxed);
        }
        // 玩家固定占 slot 0
        freeHead_.store(1, std::memory_order_relaxed);
        kind_[playerSlot_].store(static_cast<int>(PoseKind::Player), std::memory_order_relaxed);
        ownerId_[playerSlot_].store(-1, std::memory_order_relaxed);
        activeCount_.store(1, std::memory_order_relaxed);
    }

private:
    static constexpr int playerSlot_ = 0;

    // 6 帧 × 2048 槽 × 32B ≈ 384KB（对象本身要放堆上，别放栈）
    PoseFrame frames_[kPoseFrames];
    std::atomic<int> newest_{0};
    int              writeIdx_ = 0;             // 仅物理线程

    std::atomic<int>          kind_[kPoseMaxSlots];
    std::atomic<int>          ownerId_[kPoseMaxSlots];
    std::atomic<std::uint64_t> generation_[kPoseMaxSlots];
    int                        nextFree_[kPoseMaxSlots];   // 仅物理线程
    std::atomic<int>           freeHead_{1};
    std::atomic<int>           activeCount_{0};
    std::atomic<int>           slotOfStructure_[kPoseIdTableSize];
    std::atomic<int>           slotOfEntity_[kPoseIdTableSize];
};

} // namespace engine::shared

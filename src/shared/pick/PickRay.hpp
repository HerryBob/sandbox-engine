#pragma once
#include <cstdint>

namespace engine::shared::pick {

// ============================================================================
// 拾取（射线）—— 与"用谁算"解耦的公共类型
// ============================================================================
//
// 一次点击会同时产生两条射线的结果：
//   ① 业务线程：体素 DDA（查 World 的方块数据）
//   ② 物理线程：Jolt CastRay（查刚体复合形状）
// 两条射线**用同一份 PickRay 快照**，结果由 PickResolver 裁决，只允许一个生效。
//
// 为什么需要裁决：方块射线看不见已打包的方块（它们已从 World 移走），
// 刚体射线看不见静态地形（Jolt 里是 chunk 复合形状，没有方块身份）。
// 两者各答一半，靠距离比较合并。

// 一次点击时的射线快照（世界坐标，方向单位向量）
struct PickRay {
    double ox = 0.0, oy = 0.0, oz = 0.0;
    double dx = 0.0, dy = -1.0, dz = 0.0;
    double maxDist = 8.0;
};

enum class HitKind : std::uint8_t {
    None = 0,   // 没打到
    Block,      // 打到静态方块（World 里存在）
    Body,       // 打到刚体上的方块
};

// 一条射线的结果。两种命中各自带"执行动作所需的最小信息"。
struct PickHit {
    HitKind kind = HitKind::None;
    double  t = 1e30;              // 沿射线的距离（格），越小越近

    // ---- kind == Block ----
    int bx = 0, by = 0, bz = 0;    // 命中的方块（世界坐标）
    int px = 0, py = 0, pz = 0;    // 放置位（命中面外一格，世界坐标）
    std::uint16_t blockType = 0;   // 命中方块的类型（放置时也用得上）

    // ---- kind == Body ----
    int structureId = -1;          // 所属结构（= 协议里的 contraptionId）
    int lx = 0, ly = 0, lz = 0;    // 命中块的**局部**坐标
    int nx = 0, ny = 0, nz = 0;    // 命中面法线（局部，单位轴）
};

enum class PickActionKind : std::uint8_t {
    None = 0,
    BreakBlock,       // 左键：破坏静态方块
    PlaceBlock,       // 右键：在静态面外放置
    BreakBodyBlock,   // 左键：拆掉刚体上的方块
    PlaceBodyBlock,   // 右键：在刚体面外放一块
};

struct PickAction {
    PickActionKind kind = PickActionKind::None;
    PickHit block;   // kind 为 Block/BreakBodyBlock 时，用它的坐标
    PickHit body;    // kind 涉及 Body 时，用它的 structureId/局部坐标/法线
};

// ============================================================================
// 裁决：两个 PickHit → 唯一动作
// ============================================================================
//
// 规则（写死，保证确定性）：
//   ① 物理探测超时（bodyProbeTimedOut）→ 只看方块（点击不能因为对端慢而丢失）
//   ② 刚体命中且严格比方块更近 → 刚体动作
//   ③ 否则只要有方块命中 → 方块动作
//   ④ 都没有 → None
//
// 关于"平局"：射线是一维的，真正的等距极少；但两边计算空间不同
// （业务 double + 体素格，物理 float + Jolt 复合形状），同一平面算出的 t
// 可能差 1e-6 量级。这里用 kTieEps 把这种噪声**确定地**判给方块，
// 避免"同一位置有时挖方块、有时拆刚体"的偶发不一致。
//   - leftButton: true = 左键（破坏），false = 右键（放置）
//   - placeType:  右键放置时使用的方块类型（左键忽略）
inline constexpr double kTieEps = 1e-3;

struct PickDecision {
    PickActionKind kind = PickActionKind::None;
};

inline PickAction resolve(const PickHit& blockHit, const PickHit& bodyHit,
                          bool bodyProbeTimedOut, bool leftButton,
                          std::uint16_t placeType) {
    PickAction out;
    out.block = blockHit;
    out.body = bodyHit;

    const bool blockOk = (blockHit.kind == HitKind::Block);
    const bool bodyOk = (!bodyProbeTimedOut && bodyHit.kind == HitKind::Body);

    // ② 刚体严格更近（留 kTieEps 给方块，见上）
    if (bodyOk && (!blockOk || bodyHit.t + kTieEps < blockHit.t)) {
        out.kind = leftButton ? PickActionKind::BreakBodyBlock : PickActionKind::PlaceBodyBlock;
        return out;
    }
    // ③ 方块
    if (blockOk) {
        out.kind = leftButton ? PickActionKind::BreakBlock : PickActionKind::PlaceBlock;
        out.block.blockType = leftButton ? blockHit.blockType : placeType;
        return out;
    }
    // ④ 都没打中
    out.kind = PickActionKind::None;
    return out;
}

} // namespace engine::shared::pick

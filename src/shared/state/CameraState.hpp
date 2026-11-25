#pragma once
#include <atomic>

#include "core/AtomicGuarantees.hpp"

namespace engine::shared {

// 相机状态（共享槽）：渲染线程（每帧调 InputSampler::tick）写，业务线程只读。
// 2026-11-24 输入并入渲染帧后没有独立输入线程，写入方就是渲染线程自身。
// 拆自原 PlayerInput（收窄依赖面）：
//   · 渲染只需相机 5 字段 → 算 MVP（updateViewProj）；
//   · 业务只需相机 5 字段 + 点击 → DDA 射线；
//   · 点击事件（clickSeq/clickButton）单独归 PlayerInput（输入→业务专用通道）。
struct CameraState {
    // B47：本槽由渲染线程每帧写入、业务线程无锁读取，必须是真无锁原子
    // （平台保证见 core/AtomicGuarantees.hpp）。

    std::atomic<double> camX{ 0.0 };
    std::atomic<double> camY{ 0.0 };
    std::atomic<double> camZ{ 0.0 };
    std::atomic<double> yaw{ 0.0 };   // 左右旋转（弧度）
    std::atomic<double> pitch{ 0.0 };  // 上下旋转（弧度）
    // B50：跨线程读取方只剩业务线程，对一致性没有强要求，写频率每帧一次 —— relaxed 就够，
    // 不需要 seq_cst 的全局顺序。B51：这几个原子同在一条缓存行，写入时会让
    // 业务端的读取缓存行失效；写频率每帧一次且读侧本来每 tick 都要读，暂不加 padding。
    // B48（已评估，本轮不改代码）：5~6 个字段各读各的，**不是一致快照** ——
    // 可能读到"新位置 + 旧朝向"，表现为一帧的视角错位。改成 seqlock 会让渲染端
    // 每帧多两次原子读 + 重试分支，而肉眼不可见的错位不值得；玩家体位置那边
    // 有 PlayerBodyState 的 seqlock（因为位置台阶肉眼可见），相机没有这个问题。
    // 视角模式（渲染线程写，业务线程读）：
    //   0 = 自由相机（WASD 飞行）
    //   1 = 第一人称（相机在玩家眼位，自身模型需隐藏，否则视线被自己的头/躯干堵住）
    //   2 = 第三人称（相机沿视线反方向后退固定距离，能看到自己）
    std::atomic<int> viewMode{ 0 };
};

} // namespace engine::shared

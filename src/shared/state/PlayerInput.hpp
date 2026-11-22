#pragma once
#include "core/SPSCQueue.hpp"
#include <atomic>

namespace engine::shared {

// 一次鼠标按下（一次点击 = 一条事件）。
// 为什么是队列，而不是"序号 + 按钮"两个原子变量：
//   ① 两个独立 relaxed 原子之间没有 happen-before（先行发生）保证，读端可能拿到
//      "新序号 + 旧按钮"，配对只能靠 x86 的 store-store 不重排兜底；
//   ② 业务侧若按序号跳号（last = seq），一个 tick 内落进两次点击会静默吞掉前一次。
// 队列把"事件"当成不可分割的整体。
// B37：原文写"满时返回 false，可下 tick 重试"—— 但鼠标回调只 push 一次、不看返回值，
//      实际是"满了就丢（32 深，正常操作几乎不可能满）"。这里按事实描述。
struct ClickEvent {
    int button = 0;   // 0 = 左键（破坏），1 = 右键（放置）
    // B39：**没有点击位置** —— 引擎假定"准星在屏幕中心"，射线方向由相机朝向决定。
    // 将来若支持鼠标指针选块，这里要加屏幕坐标与视口信息。
};

// 玩家点击输入：输入线程（GLFW 鼠标回调）push，业务线程 pop 逐条消费。
// 相机状态已拆到 CameraState（输入线程写，渲染/业务只读）。
struct PlayerInput {
    // B40：容量 32 是"32 个事件"，底层的 SPSCQueue 内部还会多留一个哨兵槽，
    // 所以实际占用是 33 项 —— 不要按 32 去算内存或边界。
    core::SPSCQueue<ClickEvent> clicks{ 32 };  // 远大于一个 tick 可能积压的点击数

    // B41：显式禁止拷贝（SPSCQueue + 原子本来就不让拷，写出来是为了读者不用推）。
    PlayerInput() = default;
    PlayerInput(const PlayerInput&) = delete;
    PlayerInput& operator=(const PlayerInput&) = delete;

    // B42：三个 toggle* 同在一条缓存行，键盘回调（main 线程）写、输入线程读，
    // 写频率是"按键次数"，伪共享代价可忽略，不加 padding。

    // F3 调试浮层开关请求：键盘回调在"按下"瞬间置 true，渲染线程 exchange(false) 消费。
    // 用请求位而不是共享 bool：输入线程只管发事件，可见性状态归渲染线程独占。
    // B38：三个 toggle* 都是"置位 + 消费"语义，**不是计数器** ——
    //       同一 tick 内连按两次只会切换一次（第二次置位时已是 true），这是有意的。
    std::atomic<bool> toggleHud{ false };

    // F4 相机模式切换请求（自由相机 ↔ 玩家跟随）：键盘回调置位，输入线程 exchange(false) 消费。
    // 模式状态归输入线程独占（相机由它写），回调只发请求。
    std::atomic<bool> togglePlayerMode{ false };

    // F5 第一/第三人称切换请求（只在玩家模式下有效）：同上，回调置位、输入线程消费。
    std::atomic<bool> toggleThirdPerson{ false };
};

} // namespace engine::shared

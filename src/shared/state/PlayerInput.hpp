#pragma once
#include "core/SPSCQueue.hpp"
#include <atomic>

namespace engine::shared {

// 一次鼠标按下（一次点击 = 一条事件）。
// 为什么是队列，而不是"序号 + 按钮"两个原子变量：
//   ① 两个独立 relaxed 原子之间没有 happen-before（先行发生）保证，读端可能拿到
//      "新序号 + 旧按钮"，配对只能靠 x86 的 store-store 不重排兜底；
//   ② 业务侧若按序号跳号（last = seq），一个 tick 内落进两次点击会静默吞掉前一次。
// 队列把"事件"当成不可分割的整体，且天然不丢（满时返回 false，可下 tick 重试）。
struct ClickEvent {
    int button = 0;   // 0 = 左键（破坏），1 = 右键（放置）
};

// 玩家点击输入：输入线程（GLFW 鼠标回调）push，业务线程 pop 逐条消费。
// 相机状态已拆到 CameraState（输入线程写，渲染/业务只读）。
struct PlayerInput {
    core::SPSCQueue<ClickEvent> clicks{ 32 };  // 容量 32：远大于一个 tick 可能积压的点击数

    // F3 调试浮层开关请求：键盘回调在"按下"瞬间置 true，渲染线程 exchange(false) 消费。
    // 用请求位而不是共享 bool：输入线程只管发事件，可见性状态归渲染线程独占。
    std::atomic<bool> toggleHud{ false };
};

} // namespace engine::shared

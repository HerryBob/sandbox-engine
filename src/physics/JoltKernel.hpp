#pragma once
// Jolt 物理内核的进程级生命周期（分配器 / Factory / 类型注册）。
//
// 为什么单独一层：Jolt 的初始化是**进程级单例**，只能做一次
// （RegisterDefaultAllocator / Factory::sInstance / RegisterTypes）。
// 引擎自己要用，装配期自检也要用 —— 两边都自己初始化就会重复注册。
// 所以统一收在这里，谁先到谁初始化，后者拿到的是同一个已就绪的内核。
//
// 这个头**不暴露任何 Jolt 类型**：调用方（main / SelfTest）不需要、也不应该
// 把 JPH:: 拖进来。真正碰 Jolt 的只有 JoltKernel.cpp 和将来的 PhysicsWorld。

namespace engine::physics {

// 初始化 Jolt 内核（幂等：重复调用直接返回 true）。
bool initJoltKernel();

// 关闭 Jolt 内核（幂等）。必须在所有 PhysicsSystem 析构之后调用。
void shutdownJoltKernel();

// 内核是否已就绪。
bool isJoltKernelReady();

// 装配期自检：初始化内核 → 建 PhysicsSystem（地面 + 一个箱子）→ 跑 100 步
// → 断言箱子落到地面顶面上。全部在一个函数内的局部对象里完成，不碰引擎的
// 物理线程、协议、世界数据，也不碰渲染。
bool selfTestJoltKernel();

} // namespace engine::physics

#pragma once
#include <atomic>
#include <cstdint>

namespace engine::core {

// 原子类型无锁保证：下列类型的 std::atomic 在当前平台必须无锁，
// 否则跨线程读写会退化成互斥锁（阻塞写入方，破坏"读不挡写"的前提）。
// 各组件只 include 本头，不再各自重复断言。
static_assert(std::atomic<double>::is_always_lock_free,
    "atomic<double> 必须无锁：TimeManager（时间缩放）与 CameraState（相机槽）的跨线程读写依赖它");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
    "atomic<uint64_t> 必须无锁：PlayerBodyState 的 seqlock 序号依赖它");

} // namespace engine::core

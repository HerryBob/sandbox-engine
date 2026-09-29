#pragma once
#include <mutex>

namespace engine::core {

// 全局日志锁：跨线程打印统一走它，避免 std::cout 输出交错。
inline std::mutex g_logMutex;

} // namespace engine::core

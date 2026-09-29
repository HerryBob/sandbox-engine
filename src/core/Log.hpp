#pragma once
#include <iostream>
#include <mutex>
#include <string>

namespace engine::core {

// 全局日志锁：跨线程打印统一走它，避免 std::cout 输出交错。
inline std::mutex g_logMutex;

// 日志级别（从低到高）。默认 Info：Debug 隐藏，渲染/调试排查时切到 Debug 开全量。
enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };
// 全局级别开关（只在启动时设置一次，跨线程只读，无需原子）。
inline LogLevel g_logLevel = LogLevel::Info;

// 线程安全日志输出：级别过滤 + 前缀 + 整条原子写 stdout。
// 用法：log(LogLevel::Info, "[业务] ...")；拼接多用 ostringstream。
inline void log(LogLevel level, const std::string& msg) {
    if (level < g_logLevel) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    switch (level) {
    case LogLevel::Debug: std::cout << "[D] "; break;
    case LogLevel::Info:  std::cout << "[I] "; break;
    case LogLevel::Warn:  std::cout << "[W] "; break;
    case LogLevel::Error: std::cout << "[E] "; break;
    }
    std::cout << msg << "\n";
}

} // namespace engine::core

// 便捷宏（全局可用）：LOG_INFO("[业务] tick=" << t) 形式可用，
// 但参数会按字面量求值——拼接请先组好 std::string 再传。
#define LOG_DEBUG(msg) ::engine::core::log(::engine::core::LogLevel::Debug, msg)
#define LOG_INFO(msg)  ::engine::core::log(::engine::core::LogLevel::Info, msg)
#define LOG_WARN(msg)  ::engine::core::log(::engine::core::LogLevel::Warn, msg)
#define LOG_ERROR(msg) ::engine::core::log(::engine::core::LogLevel::Error, msg)

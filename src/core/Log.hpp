#pragma once
#include <string>

namespace engine::core {

// 日志级别（从低到高）。Debug 用于开发期细节，Info 为默认，Warn/Error 用于异常。
enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// 初始化日志系统：控制台（stdout，UTF-8）+ 文件（logs/engine.log，10MB × 5 滚动）。
// 必须在 chdirToExeDir() 之后调用（日志目录相对 exe 工作目录），且先于任何 LOG_* 调用。
// 重复调用无副作用；初始化失败（如日志目录不可写）时仍保留控制台输出。
void initLog(LogLevel level = LogLevel::Info);

// 刷新并停止日志后台线程。放在进程退出前（run() 返回之后）。
void shutdownLog();

// 解析等级字符串（debug/info/warn/error，大小写不敏感）；无法识别时返回 fallback。
LogLevel parseLogLevel(const std::string& text, LogLevel fallback = LogLevel::Info);

// 统一日志出口：tag 是模块名（Vulkan / Mod / Noise / ColumnGen / Model / Atlas / Input / Shader）。
// 门面内部负责等级过滤、格式化（时间/等级/线程/tag）与 sink 分发。
void log(LogLevel level, const char* tag, const std::string& msg);

} // namespace engine::core

// 便捷宏：LOG_INFO("Vulkan", std::string("x=") + std::to_string(x))
// 拼接请先组好 std::string 再传（宏只做转发，不做流式拼接）。
#define LOG_DEBUG(tag, msg) ::engine::core::log(::engine::core::LogLevel::Debug, tag, msg)
#define LOG_INFO(tag, msg)  ::engine::core::log(::engine::core::LogLevel::Info, tag, msg)
#define LOG_WARN(tag, msg)  ::engine::core::log(::engine::core::LogLevel::Warn, tag, msg)
#define LOG_ERROR(tag, msg) ::engine::core::log(::engine::core::LogLevel::Error, tag, msg)

#pragma once
#include <string>

namespace engine::core {

// 日志级别（从低到高）。Debug 用于开发期细节，Info 为默认，Warn/Error 用于异常。
enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void logInit(LogLevel level = LogLevel::Info);

void logShutdown();
// 解析等级字符串（debug/info/warn/error，大小写不敏感）；无法识别时返回 fallback。
LogLevel logParseLevel(const std::string& text, LogLevel fallback = LogLevel::Info);

// 设置当前线程在日志中的标识（如 "main" / "render" / "physics" / "worldgen-stream"）。
// 只影响调用它的那个线程；未设置的线程显示 "?"。传入内容会被拷贝保存，可安全传临时字符串。
// 传 nullptr 或空串会复位为 ?
void logSetThreadName(const char* name);

// 统一日志出口：tag 是模块名（Vulkan / Mod / Noise / ColumnGen / Model / Atlas / Input / Shader）。
// 门面内部负责等级过滤、格式化（时间/等级/线程/tag）与 sink 分发。
void log(LogLevel level, const char* tag, const std::string& msg);

} // namespace engine::core

// 便捷宏：ENGINE_LOG_INFO("Vulkan", std::string("x=") + std::to_string(x))
// 拼接请先组好 std::string 再传（宏只做转发，不做流式拼接）。
#define ENGINE_LOG_DEBUG(tag, msg) ::engine::core::log(::engine::core::LogLevel::Debug, tag, msg)
#define ENGINE_LOG_INFO(tag, msg)  ::engine::core::log(::engine::core::LogLevel::Info, tag, msg)
#define ENGINE_LOG_WARN(tag, msg)  ::engine::core::log(::engine::core::LogLevel::Warn, tag, msg)
#define ENGINE_LOG_ERROR(tag, msg) ::engine::core::log(::engine::core::LogLevel::Error, tag, msg)

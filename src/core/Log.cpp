// 日志门面的唯一实现点：本项目只有这里 include spdlog。
// 业务代码只依赖 core/Log.hpp，将来换日志库/换平台 sink 只改本文件。
#include "core/Log.hpp"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace engine::core {
namespace {

// 初始化/关闭互斥：只在启动与退出各一次，不参与日志热路径。
std::mutex g_initMutex;
std::shared_ptr<spdlog::logger> g_logger;

// LogLevel 与 spdlog 的等级枚举数值不同（spdlog 中 trace=0、debug=1、info=2、warn=3、err=4），
// 必须显式映射，不能直接 static_cast。
spdlog::level::level_enum toSpdLevel(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return spdlog::level::debug;
    case LogLevel::Info:  return spdlog::level::info;
    case LogLevel::Warn:  return spdlog::level::warn;
    case LogLevel::Error: return spdlog::level::err;
    }
    return spdlog::level::info;
}

} // namespace

void initLog(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_logger != nullptr) return;

#ifdef _WIN32
    // 日志文本按 UTF-8 输出；控制台代码页不切到 65001 中文会乱码。
    SetConsoleOutputCP(CP_UTF8);
#endif

    // 日志目录：工作目录已由 chdirToExeDir 固定到 exe 目录。
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);

    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_level(spdlog::level::trace);

    // 文件 sink：单文件 10MB × 5 个滚动副本；打不开时降级为仅控制台。
    spdlog::sink_ptr fileSink = nullptr;
    try {
        fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "logs/engine.log", 10 * 1024 * 1024, 5);
        fileSink->set_level(spdlog::level::trace);
    } catch (const spdlog::spdlog_ex&) {
        fileSink = nullptr;
    }

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(consoleSink);
    if (fileSink != nullptr) sinks.push_back(fileSink);

    // 异步写：队列 8192，满时阻塞（宁可慢一点也不丢日志）。
    // 后台线程只有 1 个，日志量低（启动/事件级），不会成为瓶颈。
    spdlog::init_thread_pool(8192, 1);
    auto logger = std::make_shared<spdlog::async_logger>(
        "engine", sinks.begin(), sinks.end(), spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);

    // 每行：[时间][等级][线程] [tag] 消息（tag 由 log() 拼进消息体）。
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%l][%t] %v");
    logger->set_level(toSpdLevel(level));
    logger->flush_on(spdlog::level::warn);

    g_logger = logger;
    spdlog::set_default_logger(logger);
}

void shutdownLog() {
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_logger == nullptr) return;
    spdlog::shutdown();   // 停止线程池并 flush 剩余队列
    g_logger.reset();
}

LogLevel parseLogLevel(const std::string& text, LogLevel fallback) {
    std::string lower;
    lower.reserve(text.size());
    for (char c : text) {
        lower.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    if (lower == "debug") return LogLevel::Debug;
    if (lower == "info")  return LogLevel::Info;
    if (lower == "warn")  return LogLevel::Warn;
    if (lower == "error") return LogLevel::Error;
    return fallback;
}

void log(LogLevel level, const char* tag, const std::string& msg) {
    const char* shownTag = (tag != nullptr && tag[0] != '\0') ? tag : "?";
    if (g_logger == nullptr) {
        // 未初始化（理论上 main 已 initLog）：退回控制台，保持可用。
        std::cout << "[" << shownTag << "] " << msg << "\n";
        return;
    }
    g_logger->log(toSpdLevel(level), "[{}] {}", shownTag, msg);
}

} // namespace engine::core

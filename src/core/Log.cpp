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
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

namespace engine::core {
namespace {

// 初始化/关闭互斥：只在启动与退出各一次，不参与日志热路径。
std::mutex g_initMutex;
std::atomic<std::shared_ptr<spdlog::logger>> g_logger{ nullptr };

// 每个线程独立一份线程名（thread_local），由 ThreadLoop::start 或 main 登记。
// 存 std::string 而非裸指针，调用方传临时字符串也不会悬垂。
thread_local std::string g_threadName = "?";

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

void logInit(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_logger.load(std::memory_order_acquire) != nullptr) return;

#ifdef _WIN32
    // 日志文本按 UTF-8 输出；控制台代码页不切到 65001 中文会乱码。
    SetConsoleOutputCP(CP_UTF8);
    // 关掉控制台"快速编辑模式"：该模式下用户在控制台里点一下/选中文字，
    // Windows 会挂起所有对该控制台的写入 → 日志后台线程被永久阻塞
    // （配合异步队列溢出策略就会冻住整个引擎，2026-11-27 实测踩到）。
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode))
            SetConsoleMode(hOut, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
    }
#endif
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    if (ec) std::cerr << "[log] cannot create log directory: " << ec.message() << "\n";

    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_level(spdlog::level::trace);

    // 文件 sink：单文件 10MB × 5 个滚动副本；打不开时降级为仅控制台。
    spdlog::sink_ptr fileSink = nullptr;
    try {
        fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "logs/engine.log", 10 * 1024 * 1024, 5);
        fileSink->set_level(spdlog::level::trace);
    }
    catch (const spdlog::spdlog_ex& e) {
        std::cerr << "[log] file sink disabled: " << e.what() << "\n";
        fileSink = nullptr;
    }

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(consoleSink);
    if (fileSink != nullptr) sinks.push_back(fileSink);

    // 异步写：队列 8192，满时阻塞（宁可慢一点也不丢日志）。
    // 后台线程只有 1 个，日志量低（启动/事件级），不会成为瓶颈。
    try {
        spdlog::init_thread_pool(8192, 1);
        auto logger = std::make_shared<spdlog::async_logger>(
            "engine", sinks.begin(), sinks.end(), spdlog::thread_pool(),
            // **不能改成 block**：队列满时会让调用线程一直等后台线程。
            // 而后台线程在写 stdout，一旦控制台进入"快速编辑/选中文本"模式，
            // Windows 会把对 stdout 的写入挂起 → 后台线程永久阻塞 → 队列填满 →
            // 所有调用 ENGINE_LOG_* 的线程（业务/物理/渲染/主线程）全部卡死，
            // 表现为"画面停住、所有输入无响应、CPU 接近 0"（2026-11-27 实测踩到）。
            // 丢日志远好过冻死引擎：满时丢弃并在下一行里用 dropped 计数暴露出来。
            spdlog::async_overflow_policy::overrun_oldest);

        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%l][%t]%v");
        logger->set_level(toSpdLevel(level));
        logger->flush_on(spdlog::level::warn);

        g_logger.store(logger, std::memory_order_release);
        spdlog::set_default_logger(logger);
    }
    catch (const std::exception& e) {
        // 回滚：不能留下"线程池已建、g_logger 却非空/半初始化"的状态
        g_logger.store(nullptr, std::memory_order_release);
        spdlog::shutdown();          // 若线程池已建，关掉它；没建则无害
        std::cerr << "[log] init failed, fallback to stderr: " << e.what() << "\n";
    }
}

void logShutdown() {
    std::lock_guard<std::mutex> lock(g_initMutex);
    auto old = g_logger.exchange(nullptr, std::memory_order_acq_rel);
    if (!old) return;
    spdlog::shutdown();
}
//大小写处理：debug/info/warn/error，无法识别时返回 fallback。
LogLevel logParseLevel(const std::string& text, LogLevel fallback) {
	//去除前后空白字符
    size_t b = 0, e = text.size();
    auto isWs = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        };
    while (b < e && isWs(text[b])) ++b;
    while (e > b && isWs(text[e - 1])) --e;
	// 转小写
    std::string lower;
    lower.reserve(e - b);
    for (size_t i = b; i < e; ++i) {             // ← 遍历 [b, e)
        char c = text[i];
        lower.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    if (lower == "debug") return LogLevel::Debug;
    if (lower == "info")  return LogLevel::Info;
    if (lower == "warn")  return LogLevel::Warn;
    if (lower == "error") return LogLevel::Error;
    return fallback;
}
//确定日志发生线程名
void logSetThreadName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        g_threadName = "?";
        return;
    }
    g_threadName = name;   // 拷贝保存，不持有调用方指针
}

void log(LogLevel level, const char* tag, const std::string& msg) {

    const char* shownTag = (tag != nullptr && tag[0] != '\0') ? tag : "?";
    const std::string line = "[" + g_threadName + "][" + shownTag + "] " + msg;
    auto lg = g_logger.load(std::memory_order_acquire);
    if (!lg) {
        std::cerr << "[uninit] " << line << std::endl;
        return;
    }
    lg->log(toSpdLevel(level), line);
}

} // namespace engine::core

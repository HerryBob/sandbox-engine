#pragma once

#include <cstdint>

namespace modapi {

// 引擎 API 版本：v4（v4 = ModApi 末尾追加 WorldApi，世界查询）。
// 契约演进只允许在结构末尾追加字段（ABI 纪律），
// 老模组若未重编，version 校验会拒绝加载，不会带着错位布局继续跑。
inline constexpr std::uint32_t kEngineApiVersion = 4;

inline constexpr std::int32_t kLogDebug = 0;
inline constexpr std::int32_t kLogInfo  = 1;
inline constexpr std::int32_t kLogWarn  = 2;
inline constexpr std::int32_t kLogError = 3;

// 共用基类：每个模组都能拿到的基础能力（版本 / 引擎上下文 / 日志）。
// 纯 POD 继承（无虚函数）：派生结构的内存布局 = 基类字段在前 + 派生字段在后，
// 跨 DLL 同编译器安全 —— "需要什么就继承什么"的基座。
struct ApiBase {
    std::uint32_t version;                        // 引擎 API 版本（模组第一件事校验它）
    void* host;                                   // 引擎侧上下文，模组不认识、原样传回
    void (*log)(std::int32_t level, const char* msg);  // 统一日志（引擎实现，模组调用）
};

} // namespace modapi

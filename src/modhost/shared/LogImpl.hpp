#pragma once

#include <cstdint>

namespace engine::modhost {

// 模组日志入口：把 modapi 的日志级别映射到引擎日志，统一标签 [Mod]。
// level：modapi::kLogDebug / kLogInfo / kLogWarn / kLogError
// msg  ：模组给出的消息，空指针直接丢弃
void logImpl(std::int32_t level, const char* msg);

} // namespace engine::modhost

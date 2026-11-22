#include "modhost/shared/LogImpl.hpp"

#include "core/Log.hpp"
#include "modapi/api_base.h"
#include "modhost/shared/HostContext.hpp"   // B6：g_host.currentMod

#include <string>

namespace engine::modhost {

void logImpl(std::int32_t level, const char* msg) {
    if (msg == nullptr) return;
    engine::core::LogLevel lv = engine::core::LogLevel::Info;
    if (level <= kLogDebug)      lv = engine::core::LogLevel::Debug;
    else if (level == kLogInfo)  lv = engine::core::LogLevel::Info;
    else if (level == kLogWarn)  lv = engine::core::LogLevel::Warn;
    else                                 lv = engine::core::LogLevel::Error;
    // B6：tag 带上模组名 —— 原来固定 "Mod"，多个模组的日志混在一起无法归属。
    // 用 currentMod（onLoad/onUnload 期间由 loader 设置；模组空闲期为空，退回 "Mod"）。
    const std::string tag = g_host.currentMod.empty()
        ? std::string("Mod")
        : ("Mod:" + g_host.currentMod);
    engine::core::log(lv, tag.c_str(), msg);
}

} // namespace engine::modhost

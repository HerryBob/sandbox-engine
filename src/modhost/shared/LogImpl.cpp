#include "modhost/shared/LogImpl.hpp"

#include "core/Log.hpp"
#include "modapi/api_base.h"

namespace engine::modhost {

void logImpl(std::int32_t level, const char* msg) {
    if (msg == nullptr) return;
    engine::core::LogLevel lv = engine::core::LogLevel::Info;
    if (level <= modapi::kLogDebug)      lv = engine::core::LogLevel::Debug;
    else if (level == modapi::kLogInfo)  lv = engine::core::LogLevel::Info;
    else if (level == modapi::kLogWarn)  lv = engine::core::LogLevel::Warn;
    else                                 lv = engine::core::LogLevel::Error;
    engine::core::log(lv, "Mod", msg);
}

} // namespace engine::modhost

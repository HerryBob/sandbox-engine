#include "modhost/noise/NoiseApi.hpp"

#include "core/Log.hpp"
#include "modhost/shared/HostContext.hpp"
#include "worldgen/noise/NoiseRegistry.hpp"

#include <cstdint>
#include <string>

namespace engine::modhost {

std::int32_t registerNoiseImpl(void* host, const modapi::NoiseProviderDef* def) {
    if (def == nullptr) {
        LOG_ERROR("Mod", "registerNoise 被拒绝：def 为空");
        return -1;
    }
    const char* type = (def->type != nullptr) ? def->type : "<null>";
    if (engine::worldgen::NoiseRegistry::frozen()) {
        LOG_ERROR("Mod", std::string("registerNoise 被拒绝：噪声注册表已冻结，加载顺序错误（")
                  + type + "）");
        return -1;
    }
    const HostContext* ctx = static_cast<const HostContext*>(host);
    const char* owner = (ctx != nullptr) ? ctx->currentMod.c_str() : nullptr;
    if (!engine::worldgen::NoiseRegistry::registerType(*def, owner)) {
        LOG_ERROR("Mod", std::string("registerNoise 失败：type 非法或重复（") + type + "）");
        return -1;
    }
    LOG_INFO("Mod", std::string("噪声注册：") + type);
    return 0;
}

} // namespace engine::modhost

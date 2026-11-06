#include "worldgen/noise/NoiseRegistry.hpp"

#include "core/Log.hpp"

namespace engine::worldgen {

std::unordered_map<std::string, modapi::NoiseProviderDef>& NoiseRegistry::table() {
    static std::unordered_map<std::string, modapi::NoiseProviderDef> entries;
    return entries;
}

bool& NoiseRegistry::frozenFlag() {
    static bool frozen = false;
    return frozen;
}

bool NoiseRegistry::registerType(const modapi::NoiseProviderDef& def) {
    if (frozenFlag()) return false;
    if (def.type == nullptr || def.type[0] == '\0') return false;
    if (def.create == nullptr || def.sample == nullptr || def.destroy == nullptr) return false;

    // key 必须持有独立拷贝（DLL 卸载后原字符串会悬空）；
    // 表内也不再保存 def.type 指针（同样理由：它指向 DLL 的静态字符串）。
    modapi::NoiseProviderDef stored = def;
    stored.type = nullptr;
    return table().emplace(std::string(def.type), stored).second;
}

bool NoiseRegistry::make(const char* type, const modapi::NoiseParamC* params,
                         std::int32_t count, NoiseHandle& out) {
    out.reset();
    if (type == nullptr) {
        LOG_ERROR("Noise", "make 收到空的 type");
        return false;
    }
    const auto& entries = table();
    const auto it = entries.find(type);
    if (it == entries.end()) {
        LOG_ERROR("Noise", std::string("未注册的噪声类型：") + type +
                               "（引擎不认识任何 type 字符串，一律查注册表）");
        return false;
    }

    const char* err = nullptr;
    void* self = it->second.create(params, count, &err);
    if (self == nullptr) {
        if (err != nullptr) {
            LOG_ERROR("Noise", std::string(type) + " 创建失败：" + err);
        } else {
            LOG_ERROR("Noise", std::string(type) + " 创建失败（provider 未给出原因）");
        }
        return false;
    }
    // 所有权转移：句柄析构时调 DLL 的 destroy（谁创建谁销毁）。
    out = NoiseHandle(self, it->second.sample, it->second.destroy);
    return true;
}

bool NoiseRegistry::contains(const char* type) {
    if (type == nullptr) return false;
    return table().find(type) != table().end();
}

std::size_t NoiseRegistry::size() {
    return table().size();
}

std::vector<std::string> NoiseRegistry::types() {
    std::vector<std::string> names;
    names.reserve(table().size());
    for (const auto& entry : table()) names.push_back(entry.first);
    return names;
}

void NoiseRegistry::freeze() {
    frozenFlag() = true;
}

bool NoiseRegistry::frozen() {
    return frozenFlag();
}

} // namespace engine::worldgen

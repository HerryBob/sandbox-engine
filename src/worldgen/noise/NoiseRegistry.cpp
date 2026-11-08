#include "worldgen/noise/NoiseRegistry.hpp"

#include "core/Log.hpp"

namespace engine::worldgen {

std::unordered_map<std::string, modapi::NoiseProviderDef>& NoiseRegistry::table() {
    static std::unordered_map<std::string, modapi::NoiseProviderDef> entries;
    return entries;
}

std::unordered_map<std::string, std::string>& NoiseRegistry::owners() {
    static std::unordered_map<std::string, std::string> entries;
    return entries;
}

bool& NoiseRegistry::frozenFlag() {
    static bool frozen = false;
    return frozen;
}

bool NoiseRegistry::registerType(const modapi::NoiseProviderDef& def, const char* owner) {
    if (frozenFlag()) return false;
    if (def.type == nullptr || def.type[0] == '\0') return false;
    if (def.create == nullptr || def.sample == nullptr || def.destroy == nullptr) return false;

    // key 必须持有独立拷贝（DLL 卸载后原字符串会悬空）；
    // 表内也不再保存 def.type 指针（同样理由：它指向 DLL 的静态字符串）。
    modapi::NoiseProviderDef stored = def;
    stored.type = nullptr;
    const std::string typeName(def.type);
    if (!table().emplace(typeName, stored).second) return false;
    owners().emplace(typeName, (owner != nullptr) ? std::string(owner) : std::string());
    return true;
}

std::size_t NoiseRegistry::eraseByOwner(const char* owner) {
    const std::string name = (owner != nullptr) ? std::string(owner) : std::string();
    const auto& tbl = table();
    std::size_t removed = 0;
    for (auto it = owners().begin(); it != owners().end();) {
        if (it->second == name) {
            table().erase(it->first);
            it = owners().erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    // 一致性检查：两张表的 key 必须一一对应（漏删会留下悬垂函数指针）。
    if (tbl.size() != owners().size()) {
        LOG_WARN("Noise", "噪声注册表与来源表数量不一致（" + std::to_string(tbl.size()) +
                              " vs " + std::to_string(owners().size()) + "）");
    }
    return removed;
}

std::size_t NoiseRegistry::countByOwner(const char* owner) {
    const std::string name = (owner != nullptr) ? std::string(owner) : std::string();
    std::size_t count = 0;
    for (const auto& entry : owners()) {
        if (entry.second == name) ++count;
    }
    return count;
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

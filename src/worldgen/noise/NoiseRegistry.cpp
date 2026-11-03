#include "worldgen/noise/NoiseRegistry.hpp"

#include <cstdio>

namespace engine::worldgen {

std::unordered_map<std::string, NoiseRegistry::NoiseFactory>& NoiseRegistry::factories() {
    static std::unordered_map<std::string, NoiseFactory> table;
    return table;
}

bool& NoiseRegistry::frozenFlag() {
    static bool frozen = false;
    return frozen;
}

bool NoiseRegistry::registerType(std::string_view type, NoiseFactory factory) {
    if (frozenFlag()) return false;
    if (type.empty() || !factory) return false;
    // string_view 只是引用，key 必须持有独立拷贝（生命周期不依赖调用方）。
    return factories().emplace(std::string(type), std::move(factory)).second;
}

std::unique_ptr<NoiseSource> NoiseRegistry::make(std::string_view type,
                                                 const NoiseParams& params) {
    auto& table = factories();
    const auto it = table.find(std::string(type));
    if (it == table.end()) {
        std::fprintf(stderr,
                     "[Noise] 未注册的噪声类型：%s（引擎不认识任何 type 字符串，一律查注册表）\n",
                     std::string(type).c_str());
        return nullptr;
    }
    // 工厂实例化：参数解析/校验在工厂内部（内置工厂读 NoiseParams.values）。
    return it->second(params);
}

void NoiseRegistry::freeze() {
    frozenFlag() = true;
}

bool NoiseRegistry::frozen() {
    return frozenFlag();
}

} // namespace engine::worldgen

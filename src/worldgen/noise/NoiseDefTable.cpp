#include "worldgen/noise/NoiseDefTable.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace engine::worldgen {

resource::DataRegistry<NoiseDef> g_noiseDefs;

// type / base 单读，其余键按值类型收进 params
bool decodeNoiseDef(const rapidjson::Value& v, NoiseDef& out, const std::string& rel) {
    (void)rel;   // 当前不需要逐条定位，失败由调用方统一报路径
    if (!v.IsObject() || !v.HasMember("type") || !v["type"].IsString()) return false;

    NoiseDef tmp;
    tmp.type = v["type"].GetString();
    if (v.HasMember("base") && v["base"].IsString()) {
        tmp.base = v["base"].GetString();
    }

    for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
        const std::string key = it->name.GetString();
        if (key == "type" || key == "base") continue;
        if (it->value.IsString()) {
            tmp.params.push_back(HeightParam{key, it->value.GetString(), 0.0});
        } else if (it->value.IsNumber()) {
            tmp.params.push_back(HeightParam{key, std::string{}, it->value.GetDouble()});
        }
    }

    out = std::move(tmp);
    return true;
}

void loadNoiseDefs() {
    g_noiseDefs.clear();
    g_noiseDefs.load(resource::ResourceKind::Data, "worldgen/noise", ".json",
                     &decodeNoiseDef, "Data");
}

} // namespace engine::worldgen

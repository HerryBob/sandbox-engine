#include "worldgen/preset/PresetLoader.hpp"

#include <rapidjson/document.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace engine::worldgen {

bool PresetLoader::load(const std::string& name, WorldPreset& out) {
    // ---- 1. 读文件：data/worldgen/presets/<name>.json ----
    // chdirToExeDir 保证相对路径从 exe 目录解析（data/ 由 CMake 构建后拷贝）。
    const std::filesystem::path path =
        std::filesystem::path("data") / "worldgen" / "presets" / (name + ".json");
    std::ifstream in(path);
    if (!in) return false;

    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();   // 具名变量：生命周期须覆盖 doc 使用期（RapidJSON 字符串引用源缓冲）

    // ---- 2. 解析 JSON ----
    rapidjson::Document doc;
    if (doc.Parse(text.c_str()).HasParseError()) return false;

    // ---- 3. 直接读字段：seed ----
    out.seed = doc["seed"].GetUint64();

    // ---- 4. heightmap：噪声参数 + 高度映射参数 ----
    const auto& hm = doc["heightmap"];
    out.baseHeight = hm["baseHeight"].GetDouble();
    out.amplitude  = hm["amplitude"].GetDouble();

    // ---- 5. 高度场参数：type/base 单读；其余键全部收进 heightParams（开放数值面）----
    const auto& noiseObj = hm["noise"];
    out.heightType = noiseObj["type"].GetString();
    if (noiseObj.HasMember("base")) {
        out.heightBase = noiseObj["base"].GetString();
    }
    for (auto it = noiseObj.MemberBegin(); it != noiseObj.MemberEnd(); ++it) {
        const std::string key = it->name.GetString();
        if (key == "type" || key == "base") continue;
        out.heightParams.push_back(HeightParam{key, std::string{}, it->value.GetDouble()});
    }

    // ---- 6. strata ----
    for (rapidjson::SizeType i = 0; i < doc["strata"].Size(); ++i) {
        const auto& s = doc["strata"][i];
        out.strata.push_back(
            Stratum{s["block"].GetString(), s["depthMin"].GetInt(), s["depthMax"].GetInt()});
    }
    return true;
}

} // namespace engine::worldgen

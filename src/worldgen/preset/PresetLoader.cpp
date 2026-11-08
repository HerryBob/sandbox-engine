#include "worldgen/preset/PresetLoader.hpp"

#include "core/Log.hpp"
#include "resource/ResourceIndex.hpp"
#include "worldgen/noise/NoiseDef.hpp"
#include "worldgen/noise/NoiseDefTable.hpp"

#include <rapidjson/document.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace engine::worldgen {
namespace fs = std::filesystem;

namespace {

// 读 JSON 文件：text 由调用方持有 —— RapidJSON 的字符串引用源缓冲，生命周期须覆盖 doc
bool readJsonFile(const fs::path& path, std::string& text, rapidjson::Document& doc) {
    std::ifstream in(path);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return !doc.Parse(text.c_str()).HasParseError();
}

} // namespace

// name：预设名（不带 .json）；失败返回 false 且不改动 out
bool PresetLoader::load(const std::string& name, WorldPreset& out) {
    WorldPreset tmp;

    // ---- 1. 查索引取预设：worldgen/presets/<name>.json（引擎与模组资源已合并进同一张表）----
    const std::string presetRel = "worldgen/presets/" + name + ".json";
    fs::path presetPath;
    if (!resource::g_resourceIndex.find(resource::ResourceKind::Data, presetRel, presetPath)) {
        LOG_ERROR("Preset", "预设未找到：" + presetRel);
        return false;
    }

    std::string presetText;
    rapidjson::Document doc;
    if (!readJsonFile(presetPath, presetText, doc)) {
        LOG_ERROR("Preset", "预设解析失败：" + presetPath.string());
        return false;
    }

    // ---- 2. seed ----
    tmp.seed = doc["seed"].GetUint64();

    // ---- 3. heightmap：噪声来源 + 高度映射参数 ----
    const auto& hm = doc["heightmap"];
    tmp.baseHeight = hm["baseHeight"].GetDouble();
    tmp.amplitude  = hm["amplitude"].GetDouble();

    // noise 两种写法：字符串 = 引用已注册的 worldgen/noise/<name>.json；对象 = 内联定义
    const auto& noiseField = hm["noise"];
    NoiseDef noiseDef;
    if (noiseField.IsString()) {
        const std::string noiseName = noiseField.GetString();
        const NoiseDef* reg = g_noiseDefs.find(noiseName);
        if (reg == nullptr) {
            LOG_ERROR("Preset", "噪声定义未注册：" + noiseName + "（预设 " + name +
                                 " 引用 data/worldgen/noise/" + noiseName + ".json）");
            return false;
        }
        noiseDef = *reg;
    } else if (noiseField.IsObject()) {
        if (!decodeNoiseDef(noiseField, noiseDef, "(内联) " + name)) {
            LOG_ERROR("Preset", "内联噪声定义缺 type：" + name);
            return false;
        }
    } else {
        LOG_ERROR("Preset", "heightmap.noise 须为字符串（名字）或对象：" + name);
        return false;
    }
    tmp.heightType = noiseDef.type;
    tmp.heightBase = noiseDef.base;
    tmp.heightParams = noiseDef.params;

    // ---- 4. strata ----
    for (rapidjson::SizeType i = 0; i < doc["strata"].Size(); ++i) {
        const auto& s = doc["strata"][i];
        tmp.strata.push_back(
            Stratum{s["block"].GetString(), s["depthMin"].GetInt(), s["depthMax"].GetInt()});
    }

    out = std::move(tmp);
    return true;
}

} // namespace engine::worldgen

#include "worldgen/preset/PresetLoader.hpp"

#include "worldgen/noise/NoiseRegistry.hpp"
#include "worldgen/noise/NoiseSource.hpp"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace engine::worldgen {

namespace {

// 预设级失败统一出口：带预设名的明确报错，返回 false。
bool fail(const std::string& name, const char* msg) {
    std::fprintf(stderr, "[Preset] %s：%s\n", name.c_str(), msg);
    return false;
}

} // namespace

bool PresetLoader::load(const std::string& name, WorldPreset& out) {
    // ---- 1. 读文件：data/worldgen/presets/<name>.json ----
    // chdirToExeDir 保证相对路径从 exe 目录解析（data/ 由 CMake 构建后拷贝）。
    const std::filesystem::path path =
        std::filesystem::path("data") / "worldgen" / "presets" / (name + ".json");
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "[Preset] 无法打开：%s\n", path.string().c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();   // 具名变量：生命周期须覆盖 doc 使用期（RapidJSON 字符串引用源缓冲）

    // ---- 2. 解析 JSON ----
    rapidjson::Document doc;
    if (doc.Parse(text.c_str()).HasParseError()) {
        std::fprintf(stderr, "[Preset] %s：JSON 解析失败：%s（offset=%u）\n",
                     name.c_str(),
                     rapidjson::GetParseError_En(doc.GetParseError()),
                     static_cast<unsigned>(doc.GetErrorOffset()));
        return false;
    }
    if (!doc.IsObject()) return fail(name, "根节点须为对象");

    // ---- 3. seed（关键键）----
    if (!doc.HasMember("seed") || !doc["seed"].IsUint64())
        return fail(name, "缺少 seed（须为无符号整数）");
    const std::uint64_t seed = doc["seed"].GetUint64();

    // ---- 4. heightmap（关键键：噪声参数 + 高度映射参数）----
    if (!doc.HasMember("heightmap") || !doc["heightmap"].IsObject())
        return fail(name, "缺少 heightmap 对象");
    const auto& hm = doc["heightmap"];
    if (!hm.HasMember("noise") || !hm["noise"].IsObject())
        return fail(name, "heightmap 缺少 noise 对象");
    if (!hm.HasMember("baseHeight") || !hm["baseHeight"].IsNumber())
        return fail(name, "heightmap 缺少 baseHeight（须为数字）");
    if (!hm.HasMember("amplitude") || !hm["amplitude"].IsNumber())
        return fail(name, "heightmap 缺少 amplitude（须为数字）");
    const double baseHeight = hm["baseHeight"].GetDouble();
    const double amplitude  = hm["amplitude"].GetDouble();

    // ---- 5. 噪声参数：type/base 关键键；其余数值键全部进 values（开放数值面）----
    const auto& noiseObj = hm["noise"];
    if (!noiseObj.HasMember("type") || !noiseObj["type"].IsString())
        return fail(name, "noise 缺少 type（须为字符串）");
    NoiseParams np;
    np.type = noiseObj["type"].GetString();
    if (noiseObj.HasMember("base")) {
        if (!noiseObj["base"].IsString())
            return fail(name, "noise.base 须为字符串");
        np.base = noiseObj["base"].GetString();
    }
    for (auto it = noiseObj.MemberBegin(); it != noiseObj.MemberEnd(); ++it) {
        const std::string key = it->name.GetString();
        if (key == "type" || key == "base") continue;
        if (!it->value.IsNumber()) {
            std::fprintf(stderr, "[Preset] %s：noise 参数 %s 须为数字\n",
                         name.c_str(), key.c_str());
            return false;
        }
        np.values[key] = it->value.GetDouble();
    }
    // fbm 必须显式给 base（关键键校验：不静默回退到某个默认基底）
    if (np.type == "fbm" && np.base.empty())
        return fail(name, "fbm 缺少 base（基底噪声类型）");

    // ---- 6. 实例化（未注册类型：make 内部已打印，这里补预设上下文）----
    auto noise = NoiseRegistry::make(np.type, np);
    if (!noise) return fail(name, "高度图噪声实例化失败（type 未注册？）");

    // ---- 7. strata（关键键：非空、字段完整、区间合法）----
    if (!doc.HasMember("strata") || !doc["strata"].IsArray() || doc["strata"].Empty())
        return fail(name, "缺少 strata 数组（且不能为空）");
    std::vector<Stratum> strata;
    for (rapidjson::SizeType i = 0; i < doc["strata"].Size(); ++i) {
        const auto& s = doc["strata"][i];
        if (!s.IsObject()) return fail(name, "strata 元素须为对象");
        if (!s.HasMember("block") || !s["block"].IsString())
            return fail(name, "strata 元素缺少 block（须为字符串）");
        if (!s.HasMember("depthMin") || !s["depthMin"].IsInt())
            return fail(name, "strata 元素 depthMin 须为整数");
        if (!s.HasMember("depthMax") || !s["depthMax"].IsInt())
            return fail(name, "strata 元素 depthMax 须为整数");
        const int depthMin = s["depthMin"].GetInt();
        const int depthMax = s["depthMax"].GetInt();
        if (depthMin < 0 || depthMin > depthMax)
            return fail(name, "strata 深度区间非法（须 0 ≤ depthMin ≤ depthMax）");
        strata.push_back(Stratum{s["block"].GetString(), depthMin, depthMax});
    }

    // ---- 8. 装配：全部校验通过后才写 out（失败路由绝不留下半成品）----
    out.seed = seed;
    out.baseHeight = baseHeight;
    out.amplitude = amplitude;
    out.heightmap = std::move(noise);
    out.strata = std::move(strata);
    return true;
}

} // namespace engine::worldgen

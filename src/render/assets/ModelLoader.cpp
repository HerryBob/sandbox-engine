#include "render/assets/ModelLoader.hpp"
#include "render/assets/ModelDef.hpp"
#include "render/assets/ModelRegistry.hpp"
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <cstdio>
#include <glm/glm.hpp>

namespace engine::assets {

namespace {

// MC 方向名 → 面索引（0..5 = +X -X +Y -Y +Z -Z，与面掩码位一致）；无效返回 0xFF
std::uint8_t faceIndexOf(std::string_view s) {
    if (s == "east")  return 0;   // +X
    if (s == "west")  return 1;   // -X
    if (s == "up")    return 2;   // +Y
    if (s == "down")  return 3;   // -Y
    if (s == "south") return 4;   // +Z
    if (s == "north") return 5;   // -Z
    return 0xFF;
}

// 解析 JSON 数组 [x,y,z] → glm::vec3；缺成员/非数组 → 警告 + 返回零向量
glm::vec3 parseVec3(const rapidjson::Value& v, const char* what, const char* file) {
    if (!v.IsArray() || v.Size() < 3) {
        std::fprintf(stderr, "[Model] %s：%s 须为 3 元素数组\n", file, what);
        return glm::vec3{0.0f};
    }
    return { v[0].GetFloat(), v[1].GetFloat(), v[2].GetFloat() };
}

} // namespace

void loadBlockModels() {
    const std::filesystem::path dir = std::filesystem::path("assets") / "models" / "block";
    if (!std::filesystem::exists(dir)) {
        std::fprintf(stderr, "[Model] 目录缺失：%s\n", dir.string().c_str());
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".json") continue;
        const std::string file = entry.path().filename().string();

        std::ifstream in(entry.path());
        if (!in) {
            std::fprintf(stderr, "[Model] 无法打开：%s\n", file.c_str());
            continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();   // 具名变量：生命周期须覆盖 doc 使用期（RapidJSON 字符串值引用源缓冲）

        rapidjson::Document doc;
        if (doc.Parse(text.c_str()).HasParseError()) {
            std::fprintf(stderr, "[Model] 解析失败：%s（%s）\n", file.c_str(),
                         rapidjson::GetParseError_En(doc.GetParseError()));
            continue;
        }

        ModelDef def;

        // textures 变量表："side" → "log_side"（模型面用 "#side" 引用）
        if (doc.HasMember("textures") && doc["textures"].IsObject()) {
            for (auto it = doc["textures"].MemberBegin(); it != doc["textures"].MemberEnd(); ++it)
                if (it->value.IsString())
                    def.textures.emplace(it->name.GetString(), it->value.GetString());
        }

        // elements：盒子列表
        if (!doc.HasMember("elements") || !doc["elements"].IsArray()) {
            std::fprintf(stderr, "[Model] %s 缺 elements 数组 → 跳过\n", file.c_str());
            continue;
        }
        for (const auto& el : doc["elements"].GetArray()) {
            if (!el.IsObject()) continue;
            ModelElement me;
            if (!el.HasMember("from") || !el.HasMember("to")) {
                std::fprintf(stderr, "[Model] %s 某元素缺 from/to → 跳过该元素\n", file.c_str());
                continue;
            }
            me.from = parseVec3(el["from"], "from", file.c_str());
            me.to   = parseVec3(el["to"],   "to",   file.c_str());

            if (el.HasMember("faces") && el["faces"].IsObject()) {
                for (auto fit = el["faces"].MemberBegin(); fit != el["faces"].MemberEnd(); ++fit) {
                    const std::uint8_t idx = faceIndexOf(fit->name.GetString());
                    if (idx == 0xFF || !fit->value.IsObject()) continue;
                    ModelFace& face = me.faces[idx];
                    const auto& fv = fit->value;
                    // texture：变量引用（#top → 展开）或直接纹理名
                    if (fv.HasMember("texture") && fv["texture"].IsString()) {
                        const std::string t = fv["texture"].GetString();
                        if (!t.empty() && t[0] == '#') {
                            const auto vt = def.textures.find(t.substr(1));
                            if (vt != def.textures.end()) face.texture = vt->second;
                            else std::fprintf(stderr, "[Model] %s 未知纹理变量 %s\n",
                                              file.c_str(), t.c_str());
                        } else {
                            face.texture = t;
                        }
                    }
                    // cullface：该方向邻块不透明则剔（编码 1..6 = 面索引+1；0 = 永不剔）
                    if (fv.HasMember("cullface") && fv["cullface"].IsString()) {
                        const std::uint8_t cf = faceIndexOf(fv["cullface"].GetString());
                        face.cullFace = (cf == 0xFF) ? 0u : static_cast<std::uint8_t>(cf + 1);
                    }
                }
            }
            def.elements.push_back(std::move(me));
        }

        if (def.elements.empty()) {
            std::fprintf(stderr, "[Model] %s 无有效元素 → 跳过\n", file.c_str());
            continue;
        }
        g_modelRegistry.add(entry.path().stem().string(), std::move(def));
    }
    std::fprintf(stderr, "[Model] 加载 %zu 个方块模型\n", g_modelRegistry.size());
}

} // namespace engine::assets

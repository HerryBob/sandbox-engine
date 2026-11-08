#include "render/assets/ModelLoader.hpp"
#include "core/Log.hpp"
#include "render/assets/ModelDef.hpp"
#include "render/assets/ModelRegistry.hpp"
#include "resource/ResourceIndex.hpp"
#include <rapidjson/document.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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
glm::vec3 parseVec3(const rapidjson::Value& v, const char* what, const std::string& rel) {
    if (!v.IsArray() || v.Size() < 3) {
        LOG_WARN("Model", rel + "：" + what + " 须为 3 元素数组");
        return glm::vec3{0.0f};
    }
    return { v[0].GetFloat(), v[1].GetFloat(), v[2].GetFloat() };
}

// 一段模型 JSON → ModelDef。允许只写 parent + textures（几何由父模型提供）。
// 面纹理的 "#变量" 此处保留原样，等 parent 链合并完成后统一展开。
bool decodeModelDef(const rapidjson::Value& doc, ModelDef& def, const std::string& rel) {
    // parent：父模型名（"block/cube_all"）
    if (doc.HasMember("parent") && doc["parent"].IsString())
        def.parent = doc["parent"].GetString();

    // textures 变量表："side" → "log_side"（模型面用 "#side" 引用）
    if (doc.HasMember("textures") && doc["textures"].IsObject()) {
        for (auto it = doc["textures"].MemberBegin(); it != doc["textures"].MemberEnd(); ++it)
            if (it->value.IsString())
                def.textures.emplace(it->name.GetString(), it->value.GetString());
    }

    // elements：盒子列表（可以没有——几何来自 parent）
    if (!doc.HasMember("elements") || !doc["elements"].IsArray()) {
        if (def.parent.empty()) {
            LOG_WARN("Model", rel + " 缺 elements 且无 parent → 跳过");
            return false;
        }
        return true;
    }
    for (const auto& el : doc["elements"].GetArray()) {
        if (!el.IsObject()) continue;
        ModelElement me;
        if (!el.HasMember("from") || !el.HasMember("to")) {
            LOG_WARN("Model", rel + " 某元素缺 from/to → 跳过该元素");
            continue;
        }
        me.from = parseVec3(el["from"], "from", rel);
        me.to   = parseVec3(el["to"],   "to",   rel);

        // rotation：元素级旋转（origin 16 单位 / axis x.y.z / angle 度 / rescale）
        if (el.HasMember("rotation") && el["rotation"].IsObject()) {
            const auto& rv = el["rotation"];
            if (rv.HasMember("origin")) me.rotation.origin = parseVec3(rv["origin"], "rotation.origin", rel);
            if (rv.HasMember("axis") && rv["axis"].IsString()) {
                const std::string a = rv["axis"].GetString();
                if (!a.empty() && (a[0] == 'x' || a[0] == 'y' || a[0] == 'z')) me.rotation.axis = a[0];
                else LOG_WARN("Model", rel + " rotation.axis 须为 x/y/z");
            }
            if (rv.HasMember("angle") && rv["angle"].IsNumber())
                me.rotation.angle = rv["angle"].GetFloat();
            if (rv.HasMember("rescale") && rv["rescale"].IsBool())
                me.rotation.rescale = rv["rescale"].GetBool();
        }

        if (el.HasMember("faces") && el["faces"].IsObject()) {
            for (auto fit = el["faces"].MemberBegin(); fit != el["faces"].MemberEnd(); ++fit) {
                const std::uint8_t idx = faceIndexOf(fit->name.GetString());
                if (idx == 0xFF || !fit->value.IsObject()) continue;
                ModelFace& face = me.faces[idx];
                const auto& fv = fit->value;
                // texture：原样保留（"#top" 或直接纹理名），烘焙阶段用合并后的表统一展开
                if (fv.HasMember("texture") && fv["texture"].IsString())
                    face.texture = fv["texture"].GetString();
                // cullface：该方向邻块不透明则剔（编码 1..6 = 面索引+1；0 = 永不剔）
                if (fv.HasMember("cullface") && fv["cullface"].IsString()) {
                    const std::uint8_t cf = faceIndexOf(fv["cullface"].GetString());
                    face.cullFace = (cf == 0xFF) ? 0u : static_cast<std::uint8_t>(cf + 1);
                }
            }
        }
        def.elements.push_back(std::move(me));
    }

    // 元素全被过滤掉时：有 parent 仍能继承父几何，否则非法
    if (def.elements.empty() && def.parent.empty()) {
        LOG_WARN("Model", rel + " 无有效元素且无 parent → 跳过");
        return false;
    }
    return true;
}

// ---- parent 链烘焙：注册表里存的始终是展开后的最终形态 ----

// parent 名 → 注册表键（"block/cube_all" → "cube_all"）
std::string parentKey(const std::string& parent) {
    if (parent.rfind("block/", 0) == 0) return parent.substr(6);
    return parent;
}

// 烘焙失败的兜底：一个整格立方体，六面都用 default 兜底纹理
ModelDef fallbackCube() {
    ModelDef def;
    ModelElement el;
    el.from = glm::vec3{0.0f, 0.0f, 0.0f};
    el.to   = glm::vec3{16.0f, 16.0f, 16.0f};
    for (std::size_t f = 0; f < 6; ++f) {
        el.faces[f].texture  = "default";
        el.faces[f].cullFace = static_cast<std::uint8_t>(f + 1);
    }
    def.elements.push_back(el);
    return def;
}

// 展开面的 "#变量"（用合并后的 textures 表）；未定义变量 → 空串 → 取图集兜底格
void expandFaceTextures(ModelDef& def) {
    for (auto& el : def.elements) {
        for (auto& face : el.faces) {
            if (face.texture.size() > 1 && face.texture[0] == '#') {
                const auto it = def.textures.find(face.texture.substr(1));
                face.texture = (it != def.textures.end()) ? it->second : std::string{};
            }
        }
    }
}

// 解析一条模型的 parent 链（记忆化 + 环检测）；out 是合并且未展开纹理变量的结果
bool resolveOne(const std::string& name,
                const std::map<std::string, ModelDef>& raw,
                std::map<std::string, ModelDef>& cache,
                std::set<std::string>& visiting,
                ModelDef& out) {
    const auto cached = cache.find(name);
    if (cached != cache.end()) {
        out = cached->second;
        return true;
    }

    if (!visiting.insert(name).second) {
        LOG_WARN("Model", "parent 循环引用：" + name);
        return false;
    }

    const auto self = raw.find(name);
    if (self == raw.end()) {
        LOG_WARN("Model", "parent 指向不存在的模型：" + name);
        visiting.erase(name);
        return false;
    }

    ModelDef result;
    result.parent = self->second.parent;

    if (!result.parent.empty()) {
        ModelDef parentDef;
        if (!resolveOne(parentKey(result.parent), raw, cache, visiting, parentDef)) {
            visiting.erase(name);
            return false;
        }
        result.elements = parentDef.elements;   // 自带 elements 为空时继承父几何
        result.textures = parentDef.textures;   // 纹理表先取父的
    }
    // textures 按 key 合并，子覆盖父；elements 子有则整段替换父
    for (const auto& kv : self->second.textures) result.textures[kv.first] = kv.second;
    if (!self->second.elements.empty()) result.elements = self->second.elements;

    visiting.erase(name);

    if (result.elements.empty()) {
        LOG_WARN("Model", "最终无几何（自身与父都没有 elements）：" + name);
        return false;
    }

    cache.emplace(name, result);
    out = result;
    return true;
}

// 把注册表里的模型按 parent 链烘焙成最终形态；失败的条目回退成默认立方体
void resolveModelParents() {
    // ① 原始快照（注册表对外只提供只读遍历）
    std::map<std::string, ModelDef> raw;
    for (const auto& kv : g_modelRegistry) raw.emplace(kv.first, kv.second.value);

    // ② 逐条解析
    std::map<std::string, ModelDef> cache;
    std::map<std::string, ModelDef> resolved;
    std::size_t failed = 0;
    for (const auto& kv : raw) {
        ModelDef def;
        std::set<std::string> visiting;
        if (resolveOne(kv.first, raw, cache, visiting, def)) {
            expandFaceTextures(def);
            resolved.emplace(kv.first, std::move(def));
        } else {
            resolved.emplace(kv.first, fallbackCube());
            ++failed;
        }
    }

    // ③ 写回注册表
    g_modelRegistry.forEachValue([&](const std::string& name, ModelDef& v) {
        const auto it = resolved.find(name);
        if (it != resolved.end()) v = it->second;
    });

    LOG_INFO("Model", "parent 烘焙 " + std::to_string(resolved.size()) + " 条，失败兜底 " +
                      std::to_string(failed) + " 条");

    // ④ 逐条诊断：烘焙后的元素数与关键面纹理（验收与走查用）
    for (const auto& kv : resolved) {
        const ModelDef& m = kv.second;
        std::string line = kv.first + " elems=" + std::to_string(m.elements.size());
        if (!m.elements.empty()) {
            const ModelElement& el = m.elements.front();
            line += " +X=" + el.faces[0].texture;
            line += " +Y=" + el.faces[2].texture;
            line += " -Y=" + el.faces[3].texture;
        }
        LOG_INFO("Model", line);
    }
}

} // namespace

// 扫 assets/models/block/*.json 全量解码注册（引擎与各模组合并，引擎优先），再烘焙 parent 链
void loadBlockModels() {
    g_modelRegistry.load(resource::ResourceKind::Assets, "models/block", ".json",
                         &decodeModelDef, "Model");
    resolveModelParents();
}

} // namespace engine::assets

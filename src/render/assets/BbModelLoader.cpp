#include "render/assets/BbModelLoader.hpp"
#include "core/Log.hpp"
#include <rapidjson/document.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>

namespace engine::assets {

namespace {

// bbmodel 面名 → 引擎面索引（0..5 = +X -X +Y -Y +Z -Z，与 ModelDef 一致）
const char* kFaceKeys[6] = { "east", "west", "up", "down", "south", "north" };

bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// 父骨骼名约定：头与双臂挂身体，身体本身与双腿挂根
const char* parentNameOf(const std::string& name) {
    if (name == "head")      return "body";
    if (name == "right_arm") return "body";
    if (name == "left_arm")  return "body";
    return nullptr;
}

// 枢轴接在哪一端：头/身接立方体底端（脖子、腰），四肢接顶端（肩膀、胯）
bool pivotAtTop(const std::string& name) {
    return name == "right_arm" || name == "left_arm" ||
           name == "right_leg" || name == "left_leg";
}

} // namespace

bool loadBbModel(const std::string& path, EntityModel& out) {
    std::string text;
    if (!readTextFile(path, text)) {
        LOG_WARN("BbModel", "读不到文件：" + path);
        return false;
    }

    rapidjson::Document doc;
    doc.Parse(text.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        LOG_WARN("BbModel", path + " JSON 解析失败");
        return false;
    }

    // 贴图分辨率（resolution 优先，缺省时从 textures[0] 读）
    if (doc.HasMember("resolution") && doc["resolution"].IsObject()) {
        const auto& res = doc["resolution"];
        if (res.HasMember("width")  && res["width"].IsInt())  out.texWidth  = res["width"].GetInt();
        if (res.HasMember("height") && res["height"].IsInt()) out.texHeight = res["height"].GetInt();
    }

    // 贴图相对路径
    if (doc.HasMember("textures") && doc["textures"].IsArray()) {
        for (const auto& t : doc["textures"].GetArray()) {
            if (!t.IsObject()) continue;
            if (t.HasMember("width")  && t["width"].IsInt()  && out.texWidth  == 0) out.texWidth  = t["width"].GetInt();
            if (t.HasMember("height") && t["height"].IsInt() && out.texHeight == 0) out.texHeight = t["height"].GetInt();
            if (t.HasMember("relative_path") && t["relative_path"].IsString())
                out.texturePaths.push_back(t["relative_path"].GetString());
        }
    }

    if (!doc.HasMember("elements") || !doc["elements"].IsArray()) {
        LOG_WARN("BbModel", path + " 缺 elements");
        return false;
    }

    glm::vec3 bbMin(1e9f), bbMax(-1e9f);

    // 每个元素一根骨骼：bbmodel 顶层是平铺 outliner，没有 group 层级
    for (const auto& el : doc["elements"].GetArray()) {
        if (!el.IsObject()) continue;
        if (!el.HasMember("from") || !el.HasMember("to")) continue;
        if (!el["from"].IsArray() || !el["to"].IsArray()) continue;

        EntityCube cube;
        cube.from = glm::vec3(el["from"][0].GetFloat(), el["from"][1].GetFloat(), el["from"][2].GetFloat());
        cube.to   = glm::vec3(el["to"][0].GetFloat(),   el["to"][1].GetFloat(),   el["to"][2].GetFloat());

        if (el.HasMember("faces") && el["faces"].IsObject()) {
            const auto& faces = el["faces"];
            for (int i = 0; i < 6; ++i) {
                if (!faces.HasMember(kFaceKeys[i])) continue;
                const auto& f = faces[kFaceKeys[i]];
                if (!f.IsObject() || !f.HasMember("uv") || !f["uv"].IsArray()) continue;
                const auto& uv = f["uv"];
                if (uv.Size() < 4) continue;
                cube.faces[i].u0 = uv[0].GetFloat();
                cube.faces[i].v0 = uv[1].GetFloat();
                cube.faces[i].u1 = uv[2].GetFloat();
                cube.faces[i].v1 = uv[3].GetFloat();
                if (f.HasMember("texture") && f["texture"].IsInt())
                    cube.textureIndex = f["texture"].GetInt();
            }
        }

        EntityBone bone;
        bone.name = (el.HasMember("name") && el["name"].IsString())
                    ? el["name"].GetString()
                    : ("bone" + std::to_string(out.bones.size()));
        bone.cubes.push_back(cube);

        bbMin = glm::min(bbMin, cube.from);
        bbMax = glm::max(bbMax, cube.to);

        out.bones.push_back(std::move(bone));
    }

    if (out.bones.empty()) {
        LOG_WARN("BbModel", path + " 没有可用元素");
        return false;
    }

    // 归一化基准：xz 取包围盒中心，y 取包围盒最低点（脚底）
    const glm::vec3 center((bbMin.x + bbMax.x) * 0.5f, bbMin.y, (bbMin.z + bbMax.z) * 0.5f);

    std::unordered_map<std::string, int> byName;
    for (int i = 0; i < static_cast<int>(out.bones.size()); ++i)
        byName[out.bones[i].name] = i;

    for (auto& bone : out.bones) {
        for (auto& cube : bone.cubes) {
            cube.from -= center;
            cube.to   -= center;
        }

        const EntityCube& c = bone.cubes.front();
        const float px = (c.from.x + c.to.x) * 0.5f + center.x;
        const float pz = (c.from.z + c.to.z) * 0.5f + center.z;
        const float py = pivotAtTop(bone.name) ? (c.to.y + center.y) : (c.from.y + center.y);
        bone.pivot = glm::vec3(px, py, pz) - center;

        if (const char* pn = parentNameOf(bone.name)) {
            auto it = byName.find(pn);
            if (it != byName.end()) bone.parent = it->second;
        }
    }

    LOG_INFO("BbModel", "加载 " + path
             + " 骨骼=" + std::to_string(out.bones.size())
             + " 贴图=" + std::to_string(out.texturePaths.size())
             + " 分辨率=" + std::to_string(out.texWidth) + "x" + std::to_string(out.texHeight));
    return true;
}

} // namespace engine::assets

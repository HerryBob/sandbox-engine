#include "modhost/manifest/Manifest.hpp"

#include <rapidjson/document.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace engine::modhost {
namespace {

// id 规则：2..64 字符，小写字母开头，只含小写字母 / 数字 / 下划线
bool isValidId(const std::string& s) {
    if (s.size() < 2 || s.size() > 64) return false;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

// 阶段名 → 枚举，只认 builtin / normal（大小写敏感）
bool parsePhase(const std::string& s, ModPhase& out) {
    if (s == "builtin") {
        out = ModPhase::Builtin;
        return true;
    }
    if (s == "normal") {
        out = ModPhase::Normal;
        return true;
    }
    return false;
}

} // namespace

bool ManifestReader::load(const std::string& path, ModManifest& out) {
    // ---- 1. 读文件 ----
    std::ifstream in(path);
    if (!in) return false;

    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();   // 生命周期须覆盖 doc 使用期（RapidJSON 引用源缓冲）

    // ---- 2. 解析 JSON ----
    rapidjson::Document doc;
    if (doc.Parse(text.c_str()).HasParseError()) return false;
    if (!doc.IsObject()) return false;

    // ---- 3. id（必填）----
    if (!doc.HasMember("id")) return false;
    if (!doc["id"].IsString()) return false;
    std::string id = doc["id"].GetString();
    if (!isValidId(id)) return false;

    // ---- 4. phase（可选，缺省 normal）----
    ModPhase phase = ModPhase::Normal;
    if (doc.HasMember("phase")) {
        if (!doc["phase"].IsString()) return false;
        if (!parsePhase(doc["phase"].GetString(), phase)) return false;
    }

    // ---- 5. dependencies（可选，缺省空）----
    std::vector<std::string> deps;
    if (doc.HasMember("dependencies")) {
        if (!doc["dependencies"].IsArray()) return false;
        const auto& arr = doc["dependencies"];
        for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
            if (!arr[i].IsString()) return false;
            if (!isValidId(arr[i].GetString())) return false;
            deps.push_back(arr[i].GetString());
        }
    }

    // ---- 6. 全部通过才写 out ----
    out.id = std::move(id);
    out.phase = phase;
    out.dependencies = std::move(deps);
    return true;
}

} // namespace engine::modhost

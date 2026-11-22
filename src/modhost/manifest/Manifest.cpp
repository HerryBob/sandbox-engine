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

// ============================================================================
// 清单的五段解析（规则1：原 ManifestReader::load 的内联体按职责拆出）
// 全部返回 false = 该段非法，调用方直接放弃（不产出半成品）。
// ============================================================================

// ① 读整个文件。text 的生命周期必须覆盖 doc 的使用期（RapidJSON 引用源缓冲）。
bool readFileText(const std::string& path, std::string& text) {
    std::ifstream in(path);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

// ② 解析 JSON 并要求顶层是对象；doc 由调用方持有（与 text 同生命周期）
bool parseJsonObject(const std::string& text, rapidjson::Document& doc) {
    if (doc.Parse(text.c_str()).HasParseError()) return false;
    return doc.IsObject();
}

// ③ id（必填）：存在、是字符串、符合 id 规则
bool readId(const rapidjson::Value& doc, std::string& out) {
    if (!doc.HasMember("id")) return false;
    if (!doc["id"].IsString()) return false;
    std::string id = doc["id"].GetString();
    if (!isValidId(id)) return false;
    out = std::move(id);
    return true;
}

// ④ phase（可选，缺省 normal）
bool readPhase(const rapidjson::Value& doc, ModPhase& out) {
    if (!doc.HasMember("phase")) return true;   // 缺省值由调用方给
    if (!doc["phase"].IsString()) return false;
    return parsePhase(doc["phase"].GetString(), out);
}

// ⑤ dependencies（可选，缺省空）：必须是数组，每项都是合法 id
bool readDependencies(const rapidjson::Value& doc, std::vector<std::string>& out) {
    if (!doc.HasMember("dependencies")) return true;
    if (!doc["dependencies"].IsArray()) return false;

    const auto& arr = doc["dependencies"];
    for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
        if (!arr[i].IsString()) return false;
        if (!isValidId(arr[i].GetString())) return false;
        out.push_back(arr[i].GetString());
    }
    return true;
}

} // namespace

bool ManifestReader::load(const std::string& path, ModManifest& out) {
    // ① 读文件
    std::string text;
    if (!readFileText(path, text)) return false;

    // ② JSON（doc 与 text 同作用域：RapidJSON 引用 text 的缓冲）
    rapidjson::Document doc;
    if (!parseJsonObject(text, doc)) return false;

    // ③④⑤ 逐段校验（任何一段失败都不写 out）
    std::string id;
    ModPhase phase = ModPhase::Normal;
    std::vector<std::string> deps;

    if (!readId(doc, id)) return false;
    if (!readPhase(doc, phase)) return false;
    if (!readDependencies(doc, deps)) return false;

    // ⑥ 全部通过才写 out（不产出半成品）
    out.id = std::move(id);
    out.phase = phase;
    out.dependencies = std::move(deps);
    return true;
}

} // namespace engine::modhost

#pragma once
// 统一注册表：把某个 registry 目录下的全部 JSON 解码成对象并注册，
// 之后取用只查这张表，不再碰文件。对应 MC 的 RegistryDataLoader + Registry。
//
// 分工：路径与归属由 ResourceIndex 给，对象怎么解由各模块传入的
// decoder（解码器）决定 —— 索引本身不认识 noise / model 是什么。
// assets 与 data 两侧共用这套：资源归哪一侧由调用方传的 kind 决定。
#include "core/Log.hpp"
#include "resource/ResourceIndex.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <rapidjson/document.h>

namespace engine::resource {

// 解码器：一段 JSON 值 → 对象。rel 是该条所在的相对路径（内联来源传个说明串），仅用于日志。
// 返回 false 表示该条非法。
// 形参用 Value 而不是 Document：文件来源与内联来源（预设里直接写的对象）共用同一份解码。
template <typename T>
using DataDecoder = bool (*)(const rapidjson::Value&, T&, const std::string& rel);

// 一条注册：解码结果 + 提供者
template <typename T>
struct DataEntry {
    T value;
    std::string owner;   // "engine" 或模组名
};

template <typename T>
class DataRegistry {
public:
    // 扫 kind 侧 relDir 下扩展名为 ext 的全部文件（relDir 形如 "worldgen/noise"），逐个解码注册。
    // 名字取文件名去扩展名。同名时先到先得，后到的告警并忽略。
    // tag 只用于日志前缀。
    void load(ResourceKind kind, const std::string& relDir, const std::string& ext,
              DataDecoder<T> decode, const char* tag);

    // 按名字取；未注册返回 nullptr
    const T* find(const std::string& name) const;

    // 提供者名；未注册返回空串
    std::string ownerOf(const std::string& name) const;

    // 已注册条目数
    std::size_t size() const;

    // 全部名字，字典序（日志与调试用）
    std::vector<std::string> names() const;

    // 清空（重建前调用）
    void clear();

    // 遍历：键 = 名字，值 = DataEntry<T>（含 owner）
    using const_iterator = typename std::map<std::string, DataEntry<T>>::const_iterator;
    const_iterator begin() const { return items_.begin(); }
    const_iterator end() const { return items_.end(); }

    // 可变遍历，供后处理写回（如模型 parent 烘焙）：fn(名字, T&)
    template <typename F>
    void forEachValue(F&& fn) {
        for (auto& kv : items_) fn(kv.first, kv.second.value);
    }

private:
    std::map<std::string, DataEntry<T>> items_;   // 有序，日志输出稳定
};

template <typename T>
void DataRegistry<T>::load(ResourceKind kind, const std::string& relDir,
                           const std::string& ext, DataDecoder<T> decode,
                           const char* tag) {
    const std::vector<std::string> files = g_resourceIndex.list(kind, relDir, ext);
    if (files.empty()) {
        LOG_WARN(tag, "索引中没有 " + relDir + "/*" + ext);
        return;
    }

    std::size_t loaded = 0;
    std::size_t failed = 0;
    for (const std::string& rel : files) {
        std::filesystem::path path;
        if (!g_resourceIndex.find(kind, rel, path)) {
            LOG_WARN(tag, "文件丢失：" + rel);
            ++failed;
            continue;
        }
        const std::string owner = g_resourceIndex.ownerOf(kind, rel);

        std::ifstream in(path);
        if (!in) {
            LOG_WARN(tag, "无法打开：" + rel + "（由 " + owner + " 提供）");
            ++failed;
            continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();   // 具名变量：RapidJSON 字符串值引用源缓冲

        rapidjson::Document doc;
        if (doc.Parse(text.c_str()).HasParseError()) {
            LOG_WARN(tag, "解析失败：" + rel + "（由 " + owner + " 提供）");
            ++failed;
            continue;
        }

        T value;
        if (!decode(doc, value, rel)) {
            LOG_WARN(tag, "定义非法：" + rel + "（由 " + owner + " 提供）");
            ++failed;
            continue;
        }

        const std::string name = std::filesystem::path(rel).stem().string();
        auto it = items_.find(name);
        if (it != items_.end()) {
            LOG_WARN(tag, "冲突：" + rel + " 已由 " + it->second.owner +
                            " 提供，忽略 " + owner);
            continue;
        }
        items_.emplace(name, DataEntry<T>{std::move(value), owner});
        ++loaded;
    }

    LOG_INFO(tag, "注册 " + std::to_string(loaded) + " 条（" + relDir + "），失败 " +
                  std::to_string(failed) + " 条");
}

template <typename T>
const T* DataRegistry<T>::find(const std::string& name) const {
    auto it = items_.find(name);
    return it == items_.end() ? nullptr : &it->second.value;
}

template <typename T>
std::string DataRegistry<T>::ownerOf(const std::string& name) const {
    auto it = items_.find(name);
    return it == items_.end() ? std::string{} : it->second.owner;
}

template <typename T>
std::size_t DataRegistry<T>::size() const {
    return items_.size();
}

template <typename T>
std::vector<std::string> DataRegistry<T>::names() const {
    std::vector<std::string> out;
    out.reserve(items_.size());
    for (const auto& kv : items_) out.push_back(kv.first);
    return out;
}

template <typename T>
void DataRegistry<T>::clear() {
    items_.clear();
}

} // namespace engine::resource

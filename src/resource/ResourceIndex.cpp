#include "resource/ResourceIndex.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace engine::resource {
namespace fs = std::filesystem;

// 全局唯一实例
ResourceIndex g_resourceIndex;

std::unordered_map<std::string, ResourceEntry>& ResourceIndex::table(ResourceKind kind) {
    return (kind == ResourceKind::Assets) ? assets_ : data_;
}

const std::unordered_map<std::string, ResourceEntry>& ResourceIndex::table(ResourceKind kind) const {
    return (kind == ResourceKind::Assets) ? assets_ : data_;
}

void ResourceIndex::addRoot(ResourceKind kind, std::string owner, const fs::path& root) {
    if (!fs::is_directory(root)) return;

    auto& tbl = table(kind);
    const fs::path base = fs::absolute(root);
    const std::string ownerName = owner.empty() ? std::string("engine") : owner;
    std::size_t added = 0;

    // 目录遍历用 error_code 版本：权限不足等单个条目失败不应中断整次扫描
    std::error_code ec;
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;

        const std::string rel = it->path().lexically_relative(base).generic_string();
        auto [pos, inserted] = tbl.emplace(rel, ResourceEntry{it->path(), ownerName});
        if (inserted) {
            ++added;
            continue;
        }
        LOG_WARN("Resource", "冲突：" + rel + "（已由 " + pos->second.owner +
                             " 提供，忽略 " + ownerName + "）");
    }

    LOG_INFO("Resource",
             std::string(kind == ResourceKind::Assets ? "assets" : "data") + " 根 " +
             ownerName + "：" + base.string() + "（新增 " + std::to_string(added) + " 个文件）");
}

bool ResourceIndex::find(ResourceKind kind, const std::string& relPath, fs::path& out) const {
    const auto& tbl = table(kind);
    const auto it = tbl.find(relPath);
    if (it == tbl.end()) return false;
    out = it->second.path;
    return true;
}

std::string ResourceIndex::ownerOf(ResourceKind kind, const std::string& relPath) const {
    const auto& tbl = table(kind);
    const auto it = tbl.find(relPath);
    return (it == tbl.end()) ? std::string() : it->second.owner;
}

std::vector<std::string> ResourceIndex::list(ResourceKind kind, const std::string& relDir,
                                            const std::string& ext) const {
    const auto& tbl = table(kind);

    std::string prefix = relDir;
    if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');

    std::vector<std::string> out;
    for (const auto& [rel, entry] : tbl) {
        (void)entry;
        if (rel.size() <= prefix.size()) continue;                        // 比前缀短：不在目录下
        if (rel.compare(0, prefix.size(), prefix) != 0) continue;         // 不在该目录下
        if (rel.find('/', prefix.size()) != std::string::npos) continue;  // 只列本层，不进子目录
        if (!ext.empty()) {
            if (rel.size() < ext.size()) continue;
            if (rel.compare(rel.size() - ext.size(), ext.size(), ext) != 0) continue;
        }
        out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::size_t ResourceIndex::count(ResourceKind kind) const {
    return table(kind).size();
}

void ResourceIndex::clear() {
    assets_.clear();
    data_.clear();
}

} // namespace engine::resource

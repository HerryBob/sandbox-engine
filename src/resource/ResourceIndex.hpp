#pragma once
// 资源索引：把所有资源根（引擎自带的 assets/data + 各模组目录下的同名子目录）
// 扫成一张"相对路径 → 文件"表，供贴图、模型、预设等按名字查找。
// 相对路径统一用正斜杠、含扩展名；先登记者优先，后到的同名条目告警并忽略。
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::resource {

// 资源类型：assets（贴图/模型/字体）与 data（世界预设/噪声定义）分表存放
enum class ResourceKind {
    Assets,
    Data,
};

// 一条登记：文件实际位置 + 提供者
struct ResourceEntry {
    std::filesystem::path path;   // 文件绝对路径
    std::string owner;            // 提供者名；引擎自带时为 "engine"
};

// 资源索引
class ResourceIndex {
public:
    // 递归扫描 root 下所有文件并登记，相对路径以 root 为基准。目录不存在直接返回。
    void addRoot(ResourceKind kind, std::string owner, const std::filesystem::path& root);

    // 按相对路径查找（如 "textures/block/brick.png"）：找到则写 out 并返回 true
    bool find(ResourceKind kind, const std::string& relPath,
              std::filesystem::path& out) const;

    // 条目提供者名（引擎自带为 "engine"）；未找到返回空串
    std::string ownerOf(ResourceKind kind, const std::string& relPath) const;

    // 列出 relDir 下一层中扩展名匹配的文件（如 "models/block" 配 ".json"），按字典序返回
    std::vector<std::string> list(ResourceKind kind, const std::string& relDir,
                                  const std::string& ext) const;

    // 已登记条目数
    std::size_t count(ResourceKind kind) const;

    // 清空两张表（重建索引前调用）
    void clear();

private:
    std::unordered_map<std::string, ResourceEntry>& table(ResourceKind kind);
    const std::unordered_map<std::string, ResourceEntry>& table(ResourceKind kind) const;

    std::unordered_map<std::string, ResourceEntry> assets_;   // 相对路径 → 条目
    std::unordered_map<std::string, ResourceEntry> data_;
};

// 全局唯一实例
extern ResourceIndex g_resourceIndex;

} // namespace engine::resource

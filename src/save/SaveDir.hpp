#pragma once
#include <cstdint>
#include <string>

#include "shared/chunk/ChunkData.hpp"

namespace engine::save {

// ============================================================================
// 存档目录（原型 step ②：只管"文件层"，不接游戏逻辑）
// ============================================================================
//
// 布局（见 docs\DOCS自制版\存档系统设计.md）：
//   <root>/level.json           世界元数据
//   <root>/chunks/<x>.<y>.<z>.bin   每 chunk 一个自描述二进制块
//
// 约定：
//   · **任何失败都返回 false**，不抛异常 —— 调用方（流式加载）据此走生成路径。
//     存档坏了可以让世界重新生成，绝不能让加载卡死。
//   · 写文件走 .tmp + rename（避免写到一半掉电留下半个文件）。
//   · chunk 坐标含负值，文件名直接用十进制（含 '-'），不编码、不分组。
struct LevelInfo {
    std::uint16_t formatVersion   = 1;      // 存档格式版本（与 chunk 头里的独立）
    std::uint32_t generatorVersion = 0;     // 地形算法版本：不符时告警但继续读
    std::uint64_t seed            = 0;
    std::string   presetName;               // 世界预设名（决定地形参数）
    std::string   worldName;
    double spawnX = 0.0, spawnY = 0.0, spawnZ = 0.0;
    double simTime = 0.0;                   // 存档时的模拟时间（秒）
};

class SaveDir {
public:
    explicit SaveDir(std::string root);

    const std::string& root() const noexcept { return root_; }

    // 创建 <root>/ 与 <root>/chunks/；已存在视为成功。
    bool ensureDirs() const;
    bool exists() const;

    std::string chunkPath(int cx, int cy, int cz) const;
    bool hasChunk(int cx, int cy, int cz) const;

    bool writeChunk(int cx, int cy, int cz, const engine::shared::ChunkData& chunk) const;
    bool readChunk(int cx, int cy, int cz, engine::shared::ChunkData& out) const;

    bool writeLevel(const LevelInfo& info) const;
    bool readLevel(LevelInfo& out) const;

    // 删除整个存档目录（世界重置用；原型里只会被自检和手动清理用到）
    bool removeAll() const;

private:
    std::string root_;
};

} // namespace engine::save

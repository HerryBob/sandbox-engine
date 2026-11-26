#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>

#include "save/SaveDir.hpp"
#include "shared/chunk/ChunkPos.hpp"

namespace engine::business {
class World;
}

namespace engine::save {

// ============================================================================
// SaveSession：一次游玩会话的存档写入（原型 step ④）
// ============================================================================
//
// 分工：
//   SaveDir      —— 文件层（路径、读写、原子写）
//   SaveSession  —— 会话层：记住"哪些 chunk 该写"，决定何时写
//
// 写入策略（原型）：
//   · chunk 卸载 → **先写再删**（数据马上要没了，必须立刻落盘）
//   · 新生成/从盘加载的 chunk → 记进待写集合
//   · 退出时 flush 全部待写
//
// 为什么不用"每秒定时写"：原型世界里脏数据增长慢（走动才产生），退出时写一次就够；
// 定时写要处理并发与写盘预算，等 chunk 上千再引入。
//
// 多存档（后续）：<root>/<worldName>/ 已经是命名空间结构，
// 将来加"新建/选择/删除存档"只是换 worldName 与目录管理，格式不用动。
class SaveSession {
public:
    SaveSession() = default;

    // root：存档根目录（如 E:\MinecraftGame2\saves）；worldName：存档名（如 "world"）。
    // meta 里的世界元数据会写到 level.json；已存在 level.json 时**不覆盖**
    // （种子/预设是创建时定的，重进不能改）。
    bool init(const std::filesystem::path& root, const std::string& worldName,
              const LevelInfo& meta);

    bool active() const noexcept { return active_; }
    const std::filesystem::path& worldDir() const noexcept { return worldDir_; }

    // 本会话写过的 chunk 数（诊断）
    long long savedChunkCount() const noexcept { return savedChunks_; }

    // 标记"这个 chunk 需要落盘"（新生成或从盘加载的 chunk 后续可能被改）
    void markPending(const engine::shared::ChunkPos& cp);
    std::size_t pendingCount() const noexcept { return pending_.size(); }

    // 立即写一个 chunk（卸载前调用）；world 里已经没有它了就跳过
    bool saveNow(const engine::business::World& world, const engine::shared::ChunkPos& cp);

    // 写掉所有待写 chunk；simTime 会一并写进 level.json
    bool flush(const engine::business::World& world, double simTime);

private:
    SaveDir  dir_;
    bool     active_ = false;
    std::filesystem::path worldDir_;
    std::unordered_set<engine::shared::ChunkPos> pending_;
    LevelInfo meta_;
    long long savedChunks_ = 0;
};

} // namespace engine::save

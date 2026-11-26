#include "save/SaveSession.hpp"

#include "business/World.hpp"
#include "core/Log.hpp"

namespace engine::save {

bool SaveSession::init(const std::filesystem::path& root, const std::string& worldName,
                       const LevelInfo& meta) {
    if (worldName.empty()) {
        ENGINE_LOG_ERROR("Save", "存档名为空，存档功能关闭");
        return false;
    }
    worldDir_ = root / worldName;
    dir_ = SaveDir(worldDir_.string());
    if (!dir_.ensureDirs()) return false;

    meta_ = meta;
    meta_.worldName = worldName;

    // level.json 只在"新建存档"时写：种子/预设/出生点是创建时决定的，
    // 重进时若被覆盖，"同一个存档换了地形"会变成静默的数据损坏。
    LevelInfo existing;
    if (dir_.readLevel(existing)) {
        meta_ = existing;                 // 沿用创建时的元数据
        meta_.worldName = worldName;
    } else {
        if (!dir_.writeLevel(meta_)) {
            ENGINE_LOG_ERROR("Save", "level.json 写入失败（存档仍可用，只是缺元数据）");
        }
        ENGINE_LOG_INFO("Save", "新建存档：" + worldDir_.string() +
            "（seed=" + std::to_string(meta_.seed) + " preset=" + meta_.presetName + "）");
    }

    active_ = true;
    return true;
}

void SaveSession::markPending(const engine::shared::ChunkPos& cp) {
    if (!active_) return;
    pending_.insert(cp);
}

bool SaveSession::saveNow(const engine::business::World& world,
                          const engine::shared::ChunkPos& cp) {
    if (!active_) return false;
    engine::shared::ChunkData data;
    if (!world.copyChunk(cp, data)) {
        // 世界里没有这个 chunk = 全空气（世界不为空 chunk 分配内存）。
        // **仍然要写**：否则磁盘上的列永远缺这几层，加载端"4 层必须齐全"就永远 miss。
        // 全空气走序列化的快路径，只占 32 字节。
        data = engine::shared::ChunkData{};
    }
    pending_.erase(cp);
    if (!dir_.writeChunk(cp.x, cp.y, cp.z, data)) {
        ENGINE_LOG_WARN("Save", "chunk 写盘失败：(" + std::to_string(cp.x) + "," +
                        std::to_string(cp.y) + "," + std::to_string(cp.z) + ")");
        return false;
    }
    ++savedChunks_;
    return true;
}

bool SaveSession::flush(const engine::business::World& world, double simTime) {
    if (!active_) return false;

    // 先拷一份再遍历：saveNow 会 erase 待写集合
    const std::vector<engine::shared::ChunkPos> todo(pending_.begin(), pending_.end());
    bool ok = true;
    for (const auto& cp : todo) {
        if (!saveNow(world, cp)) ok = false;
    }
    pending_.clear();

    meta_.simTime = simTime;
    if (!dir_.writeLevel(meta_)) ok = false;

    ENGINE_LOG_INFO("Save", "存档已写入：" + std::to_string(savedChunks_) +
        " 个 chunk → " + worldDir_.string());
    return ok;
}

} // namespace engine::save

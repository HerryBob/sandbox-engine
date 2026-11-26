#include "save/SaveDir.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "core/Log.hpp"

namespace engine::save {

namespace fs = std::filesystem;

namespace {

// 原子写：先写 <path>.tmp，再覆盖 rename。
// 目的：写到一半掉电/崩溃时，旧文件仍是完整的（.tmp 残片被忽略）。
bool writeFileAtomic(const std::string& path, const void* data, std::size_t size) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!f) return false;
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    fs::remove(path, ec);                 // Windows 的 rename 不覆盖已存在文件
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool readFile(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    out.resize(static_cast<std::size_t>(size));
    f.seekg(0, std::ios::beg);
    if (size > 0) {
        f.read(reinterpret_cast<char*>(out.data()), size);
        if (!f) return false;
    }
    return true;
}

bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return f.good() || f.eof();
}

} // namespace

SaveDir::SaveDir(std::string root) : root_(std::move(root)) {}

bool SaveDir::ensureDirs() const {
    std::error_code ec;
    fs::create_directories(fs::path(root_) / "chunks", ec);
    if (ec) {
        ENGINE_LOG_ERROR("Save", "创建存档目录失败：" + root_ + "（" + ec.message() + "）");
        return false;
    }
    return true;
}

bool SaveDir::exists() const {
    std::error_code ec;
    return fs::is_directory(root_, ec);
}

std::string SaveDir::chunkPath(int cx, int cy, int cz) const {
    return (fs::path(root_) / "chunks" /
            (std::to_string(cx) + "." + std::to_string(cy) + "." + std::to_string(cz) + ".bin"))
        .string();
}

bool SaveDir::hasChunk(int cx, int cy, int cz) const {
    std::error_code ec;
    return fs::is_regular_file(chunkPath(cx, cy, cz), ec);
}

bool SaveDir::writeChunk(int cx, int cy, int cz,
                         const engine::shared::ChunkData& chunk) const {
    std::vector<std::uint8_t> buf;
    // genVer 由调用方（世界层）在需要时决定；原型里 LevelInfo 的版本就是唯一来源，
    // 这里传 0 表示"未标注"，读回时按 0 处理（不告警）。
    chunk.serialize(buf, cx, cy, cz, 0);
    if (buf.empty()) return false;
    return writeFileAtomic(chunkPath(cx, cy, cz), buf.data(), buf.size());
}

bool SaveDir::readChunk(int cx, int cy, int cz, engine::shared::ChunkData& out) const {
    std::vector<std::uint8_t> buf;
    if (!readFile(chunkPath(cx, cy, cz), buf)) return false;   // 不存在 = 无存档
    return engine::shared::ChunkData::deserialize(buf.data(), buf.size(), cx, cy, cz, out);
}

bool SaveDir::writeLevel(const LevelInfo& info) const {
    rapidjson::Document d;
    d.SetObject();
    auto& alloc = d.GetAllocator();
    d.AddMember("formatVersion", info.formatVersion, alloc);
    d.AddMember("generatorVersion", info.generatorVersion, alloc);
    d.AddMember("seed", info.seed, alloc);
    d.AddMember("preset", rapidjson::Value(info.presetName.c_str(), alloc), alloc);
    d.AddMember("worldName", rapidjson::Value(info.worldName.c_str(), alloc), alloc);
    d.AddMember("spawnX", info.spawnX, alloc);
    d.AddMember("spawnY", info.spawnY, alloc);
    d.AddMember("spawnZ", info.spawnZ, alloc);
    d.AddMember("simTime", info.simTime, alloc);

    rapidjson::StringBuffer sb;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
    d.Accept(w);

    return writeFileAtomic((fs::path(root_) / "level.json").string(), sb.GetString(), sb.GetSize());
}

bool SaveDir::readLevel(LevelInfo& out) const {
    std::string text;
    if (!readTextFile((fs::path(root_) / "level.json").string(), text)) return false;

    rapidjson::Document d;
    d.Parse(text.c_str());
    if (d.HasParseError() || !d.IsObject()) {
        ENGINE_LOG_WARN("Save", "level.json 解析失败，按无存档处理");
        return false;
    }

    LevelInfo info;
    auto u16 = [&d](const char* k) -> std::uint16_t {
        return d.HasMember(k) && d[k].IsUint() ? static_cast<std::uint16_t>(d[k].GetUint()) : 0;
    };
    auto u32 = [&d](const char* k) -> std::uint32_t {
        return d.HasMember(k) && d[k].IsUint() ? d[k].GetUint() : 0u;
    };
    auto u64 = [&d](const char* k) -> std::uint64_t {
        return d.HasMember(k) && d[k].IsUint64() ? d[k].GetUint64() : 0u;
    };
    auto dbl = [&d](const char* k) -> double {
        if (d.HasMember(k) && d[k].IsNumber()) return d[k].GetDouble();
        return 0.0;
    };
    auto str = [&d](const char* k) -> std::string {
        return d.HasMember(k) && d[k].IsString() ? std::string(d[k].GetString()) : std::string();
    };

    info.formatVersion    = u16("formatVersion");
    info.generatorVersion = u32("generatorVersion");
    info.seed             = u64("seed");
    info.presetName       = str("preset");
    info.worldName        = str("worldName");
    info.spawnX           = dbl("spawnX");
    info.spawnY           = dbl("spawnY");
    info.spawnZ           = dbl("spawnZ");
    info.simTime          = dbl("simTime");
    out = std::move(info);
    return true;
}

bool SaveDir::removeAll() const {
    std::error_code ec;
    fs::remove_all(root_, ec);
    return !ec;
}

} // namespace engine::save

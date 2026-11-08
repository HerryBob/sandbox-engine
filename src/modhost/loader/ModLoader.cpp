#include "modhost/loader/ModLoader.hpp"

#include "core/Log.hpp"
#include "modhost/block/BlockApi.hpp"
#include "modhost/manifest/Manifest.hpp"
#include "modhost/noise/NoiseApi.hpp"
#include "modhost/shared/HostContext.hpp"
#include "modhost/shared/LogImpl.hpp"
#include "modhost/sorter/ModSorter.hpp"
#include "modhost/world/WorldApi.hpp"
#include "registry/BlockRegistry.hpp"
#include "resource/ResourceIndex.hpp"
#include "worldgen/noise/NoiseRegistry.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace engine::modhost {
namespace {

namespace fs = std::filesystem;

// 阶段名（日志用）
const char* phaseName(ModPhase p) {
    return (p == ModPhase::Builtin) ? "builtin" : "normal";
}

// 取模组目录内唯一的 DLL；零个或多个都返回空路径（都已记日志）
fs::path findSingleDll(const fs::path& modDir, const std::string& shown) {
    fs::path found;
    for (const auto& entry : fs::directory_iterator(modDir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".dll") continue;
        if (!found.empty()) {
            LOG_ERROR("Mod", "拒绝加载 " + shown + "：目录内存在多个 DLL");
            return fs::path{};
        }
        found = entry.path();
    }
    if (found.empty()) {
        LOG_WARN("Mod", "跳过 " + shown + "：目录内没有 DLL");
    }
    return found;
}

} // namespace

std::vector<ModLoader::LoadedMod> ModLoader::s_mods_;

std::size_t ModLoader::loadAll(const std::string& dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        LOG_INFO("Mod", "模组目录不存在，按 0 个模组继续（" + dir + "）");
        return 0;
    }

    g_api = modapi::ModApi{};
    // 共用基类字段在三个子结构各填一份（继承布局：每个子结构自带 version/host/log）
    g_api.block.version       = modapi::kEngineApiVersion;
    g_api.block.host          = &g_host;
    g_api.block.log           = &logImpl;
    g_api.block.registerBlock = &registerBlockImpl;
    g_api.noise.version       = modapi::kEngineApiVersion;
    g_api.noise.host          = &g_host;
    g_api.noise.log           = &logImpl;
    g_api.noise.registerNoise = &registerNoiseImpl;
    g_api.world.version            = modapi::kEngineApiVersion;
    g_api.world.host               = &g_host;
    g_api.world.log                = &logImpl;
    g_api.world.getBlock           = &getBlockImpl;
    g_api.world.forEachBlockInAABB = &forEachBlockInAABBImpl;
    g_api.world.getWorldInfo       = &getWorldInfoImpl;

    // 一个子目录 = 一个模组：目录内一个 *.dll + 可选的 mod.json。
    // 先收集再排序，不依赖目录枚举顺序，保证每次启动的加载次序一致。
    struct Candidate {
        fs::path    dll;          // 目录内唯一的 DLL
        std::string dirName;      // 目录名（日志标识 / 无清单时的排序键）
        bool        hasManifest;  // 目录里有没有 mod.json
        ModManifest manifest;     // hasManifest 为 true 时有效
    };

    std::vector<Candidate> candidates;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_directory()) {
            LOG_WARN("Mod", "忽略 mods/ 下的散装文件（模组须放进子目录）：" +
                             entry.path().filename().string());
            continue;
        }

        Candidate c;
        c.dirName = entry.path().filename().string();
        c.dll     = findSingleDll(entry.path(), c.dirName);
        if (c.dll.empty()) continue;

        const fs::path manifestPath = entry.path() / "mod.json";
        if (fs::exists(manifestPath)) {
            if (!ManifestReader::load(manifestPath.string(), c.manifest)) {
                LOG_ERROR("Mod", "拒绝加载 " + c.dirName + "：mod.json 非法");
                continue;
            }
            c.hasManifest = true;
            LOG_INFO("Mod", "清单：" + c.manifest.id + "（phase=" +
                             phaseName(c.manifest.phase) + "，依赖 " +
                             std::to_string(c.manifest.dependencies.size()) + " 个）");
        } else {
            c.hasManifest = false;
            LOG_INFO("Mod", "无 mod.json，按 normal 处理：" + c.dirName);
        }
        candidates.push_back(c);
    }

    // 排序：先 phase 升序，同 phase 按 id（无清单的用目录名）。
    // 这一步只固定"同层节点"的先后，真正的前后关系由依赖排序决定。
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  const ModPhase pa = a.hasManifest ? a.manifest.phase : ModPhase::Normal;
                  const ModPhase pb = b.hasManifest ? b.manifest.phase : ModPhase::Normal;
                  if (pa != pb) {
                      return static_cast<std::int32_t>(pa) < static_cast<std::int32_t>(pb);
                  }
                  const std::string& ia = a.hasManifest ? a.manifest.id : a.dirName;
                  const std::string& ib = b.hasManifest ? b.manifest.id : b.dirName;
                  return ia < ib;
              });

    // 排序输入：无清单的模组合成 id（"<dir>" + 目录名，不会与真 id 冲突），无依赖、归 normal。
    std::vector<ModManifest> items;
    items.reserve(candidates.size());
    for (const Candidate& c : candidates) {
        if (c.hasManifest) {
            items.push_back(c.manifest);
        } else {
            ModManifest m;
            m.id = "<dir>" + c.dirName;
            m.phase = ModPhase::Normal;
            items.push_back(m);
        }
    }

    const SortResult sorted = ModSorter::sort(items);
    for (const RejectedMod& r : sorted.rejected) {
        LOG_ERROR("Mod", "拒绝加载 " + candidates[r.index].dirName + "：" + r.reason);
    }

    // 加载失败的模组 id 集合：被依赖方失败时，依赖方一并跳过（运行期级联）
    std::unordered_set<std::string> failedIds;

    std::size_t okCount = 0;
    for (std::size_t idx : sorted.order) {
        const Candidate& c = candidates[idx];
        const std::string shown = c.dirName;
        const std::string& modId = items[idx].id;

        // 依赖的模组没加载成功 → 本模组也跳过，否则它要用的东西并不存在
        bool missingDep = false;
        for (const std::string& dep : items[idx].dependencies) {
            if (failedIds.count(dep) == 0) continue;
            LOG_ERROR("Mod", "跳过 " + shown + "：依赖的模组未加载（" + dep + "）");
            missingDep = true;
            break;
        }
        if (missingDep) {
            failedIds.insert(modId);
            continue;
        }

        HMODULE h = ::LoadLibraryW(c.dll.wstring().c_str());
        if (h == nullptr) {
            LOG_ERROR("Mod", "LoadLibrary 失败（GetLastError=" +
                      std::to_string(static_cast<long>(::GetLastError())) + "）：" + shown);
            failedIds.insert(modId);
            continue;
        }

        // GetProcAddress 返回 FARPROC，必须先转 void* 再转目标函数指针类型：
        // 直接 FARPROC → 函数指针在 MSVC 下会触发 C4191（不安全类型转换）。
        modapi::CreateModFn create = reinterpret_cast<modapi::CreateModFn>(
            reinterpret_cast<void*>(::GetProcAddress(h, modapi::kCreateModSymbol)));
        modapi::DestroyModFn destroy = reinterpret_cast<modapi::DestroyModFn>(
            reinterpret_cast<void*>(::GetProcAddress(h, modapi::kDestroyModSymbol)));

        if (create == nullptr || destroy == nullptr) {
            LOG_ERROR("Mod", "拒绝加载 " + shown + "：缺少 CreateMod / DestroyMod 导出符号");
            failedIds.insert(modId);
            ::FreeLibrary(h);
            continue;
        }

        // 阶段③：实例化。构造函数里不做注册（注册一律放 onLoad，方便失败回滚）
        modapi::IMod* mod = create(&g_api);
        if (mod == nullptr) {
            LOG_ERROR("Mod", "CreateMod 返回空指针：" + shown);
            failedIds.insert(modId);
            ::FreeLibrary(h);
            continue;
        }

        // 来源标记：onLoad 里注册内容时要记到这本模组名下，必须在 onLoad 之前设置。
        // 拷成 std::string —— mod->name() 指向 DLL 静态区，卸载后即失效。
        g_host.currentMod = mod->name();

        // 清单 id 与 DLL 自报名字不一致：只警告，不拒绝（排序与拒绝以清单为准）
        if (c.hasManifest && c.manifest.id != mod->name()) {
            LOG_WARN("Mod", "清单 id 与 IMod::name() 不一致：" + c.manifest.id +
                             " vs " + mod->name() + "（" + shown + "）");
        }

        // 阶段④：初始化。模组在这里校验 api->version 并注册内容。
        if (!mod->onLoad(&g_api)) {
            LOG_ERROR("Mod", "onLoad 返回 false，放弃该模组：" + shown);
            // 失败回滚：把这个模组已经注册进去的内容按来源清掉，避免残留
            // （噪声表里存的是它的函数指针，随后的 FreeLibrary 会让它们悬垂）。
            const std::size_t rolledBack = engine::worldgen::NoiseRegistry::eraseByOwner(
                g_host.currentMod.c_str());
            if (rolledBack > 0) {
                LOG_INFO("Mod", "回滚噪声注册 " + std::to_string(rolledBack) + " 个（" + shown + "）");
            }
            destroy(mod);
            g_host.currentMod.clear();
            failedIds.insert(modId);
            ::FreeLibrary(h);
            continue;
        }

        std::ostringstream oss;
        oss << "加载成功：" << mod->name() << " v" << mod->version()
            << "（" << shown << "）";
        LOG_INFO("Mod", oss.str());

        // 模组资源：目录下的 assets/ 与 data/ 并入全局索引（不存在则跳过）。
        // 失败模组不登记 —— 代码没加载，其资源也不该占用名字。
        const fs::path modRoot = c.dll.parent_path();
        engine::resource::g_resourceIndex.addRoot(
            engine::resource::ResourceKind::Assets, g_host.currentMod, modRoot / "assets");
        engine::resource::g_resourceIndex.addRoot(
            engine::resource::ResourceKind::Data, g_host.currentMod, modRoot / "data");

        s_mods_.push_back(LoadedMod{c.dll.string(), g_host.currentMod, reinterpret_cast<void*>(h), mod, destroy});
        g_host.currentMod.clear();
        ++okCount;
    }

    if (okCount == 0) {
        LOG_INFO("Mod", "本次未加载任何模组");
    } else {
        LOG_INFO("Mod", "加载 " + std::to_string(okCount) + " 个模组");
    }
    return okCount;
}

void ModLoader::unloadAll() {
    // 逆序卸载：后加载的模组可能依赖先加载的模组注册的内容，先卸它对依赖方更安全。
    for (std::size_t i = s_mods_.size(); i > 0; --i) {
        LoadedMod& m = s_mods_[i - 1U];
        if (m.instance != nullptr) {
            m.instance->onUnload();
            if (m.destroy != nullptr) m.destroy(m.instance);  // 谁创建谁销毁：delete 在 DLL 内
            m.instance = nullptr;
        }
        // 卸载闭环：把本模组注册的噪声 provider 按来源清掉。
        // 必须在 FreeLibrary 之前做 —— 表里的 create/sample/destroy 指向 DLL 代码段，
        // 库一卸就是悬垂函数指针，之后再查表或创建噪声即未定义行为。
        // 方块定义不在此时清除：它已是注册表自持的字符串/数值副本，DLL 卸载后仍自洽；
        // 且 id 已写进 chunk 数据，清掉会让世界里的方块变成查不到的 id
        // （真正要支持"卸载后重载仍认旧世界"，需要 id 复用机制，另作一件事）。
        const std::size_t removedNoise =
            engine::worldgen::NoiseRegistry::eraseByOwner(m.name.c_str());
        if (removedNoise > 0) {
            LOG_INFO("Mod", "已清理噪声注册 " + std::to_string(removedNoise) +
                                " 个（" + m.name + "）");
        }
        const std::size_t keptBlocks =
            engine::registry::g_blockRegistry.countByOwner(m.name.c_str());
        if (keptBlocks > 0) {
            LOG_INFO("Mod", "保留方块定义 " + std::to_string(keptBlocks) +
                                " 个（" + m.name + "，纯数据副本，卸载后仍可用）");
        }
        if (m.handle != nullptr) {
            ::FreeLibrary(static_cast<HMODULE>(m.handle));
            m.handle = nullptr;
        }
        LOG_INFO("Mod", "已卸载：" + m.path + "（" + m.name + "）");
    }
    s_mods_.clear();

    // 卸载闭环自检：噪声表应只剩引擎自身注册的（owner 为空），方块表按策略保留。
    // 这行是"按来源清除没有误删引擎内置内容"的直接证据。
    LOG_INFO("Mod", "卸载后注册表：方块 " +
                        std::to_string(engine::registry::g_blockRegistry.size()) +
                        " 个（按策略保留），噪声 " +
                        std::to_string(engine::worldgen::NoiseRegistry::size()) +
                        " 个（模组注册的已按来源清除）");
}

std::size_t ModLoader::loadedCount() {
    return s_mods_.size();
}

void ModLoader::attachWorld(engine::business::World* world) {
    g_host.world = world;
}

} // namespace engine::modhost

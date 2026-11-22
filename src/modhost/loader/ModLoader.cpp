#include "modhost/loader/ModLoader.hpp"

#include "core/Log.hpp"
#include "modhost/block/BlockApi.hpp"
#include "modhost/entity/EntityApi.hpp"
#include "modhost/entity/EntityDefTable.hpp"
#include "modhost/entity/EntityInstanceTable.hpp"
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
            ENGINE_LOG_ERROR("Mod", "拒绝加载 " + shown + "：目录内存在多个 DLL");
            return fs::path{};
        }
        found = entry.path();
    }
    if (found.empty()) {
        ENGINE_LOG_WARN("Mod", "跳过 " + shown + "：目录内没有 DLL");
    }
    return found;
}

} // namespace

std::vector<ModLoader::LoadedMod> ModLoader::s_mods_;

// ============================================================================
// 加载流水线的前四段（规则1 拆分：原 loadAll 的内联体按职责拆出，行为逐行不变）
// ============================================================================
namespace {

// 一个候选模组：目录内唯一的 DLL + 目录名 + 已校验的清单
struct Candidate {
    fs::path    dll;          // 目录内唯一的 DLL
    std::string dirName;      // 目录名（日志标识）
    ModManifest manifest;     // 清单（收集阶段已保证存在）
};

// ① 填引擎能力表：四个子结构各填一份 version/host/log + 能力函数。
//    必须在任何模组加载之前调用 —— 模组 onLoad 第一件事就是调 api->*.log。
void prepareModApi() {
    g_api = ModApi{};
    // 共用基类字段在三个子结构各填一份（继承布局：每个子结构自带 version/host/log）
    g_api.block.version       = kEngineApiVersion;
    g_api.block.host          = &g_host;
    g_api.block.log           = &logImpl;
    g_api.block.registerBlock = &registerBlockImpl;

    g_api.noise.version       = kEngineApiVersion;
    g_api.noise.host          = &g_host;
    g_api.noise.log           = &logImpl;
    g_api.noise.registerNoise = &registerNoiseImpl;

    g_api.world.version            = kEngineApiVersion;
    g_api.world.host               = &g_host;
    g_api.world.log                = &logImpl;
    g_api.world.getBlock           = &getBlockImpl;
    g_api.world.forEachBlockInAABB = &forEachBlockInAABBImpl;
    g_api.world.getWorldInfo       = &getWorldInfoImpl;

    g_api.entity.version        = kEngineApiVersion;
    g_api.entity.host           = &g_host;
    g_api.entity.log            = &logImpl;
    g_api.entity.registerEntity = &registerEntityImpl;
}

// ② 扫目录收集候选：一个子目录 = 一个模组（一个 *.dll + 一个必需的 mod.json）。
//    散装文件 / DLL 数量不对 / 缺清单 / 清单非法 —— 全部在这里跳过并记日志。
std::vector<Candidate> collectCandidates(const std::string& dir) {
    std::vector<Candidate> candidates;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_directory()) {
            ENGINE_LOG_WARN("Mod", "忽略 mods/ 下的散装文件（模组须放进子目录）：" +
                             entry.path().filename().string());
            continue;
        }

        Candidate c;
        c.dirName = entry.path().filename().string();
        c.dll     = findSingleDll(entry.path(), c.dirName);
        if (c.dll.empty()) continue;

        const fs::path manifestPath = entry.path() / "mod.json";
        if (!fs::exists(manifestPath)) {
            ENGINE_LOG_WARN("Mod", "跳过 " + c.dirName + "：缺少 mod.json（模组必须有清单）");
            continue;
        }
        if (!ManifestReader::load(manifestPath.string(), c.manifest)) {
            ENGINE_LOG_ERROR("Mod", "拒绝加载 " + c.dirName + "：mod.json 非法");
            continue;
        }
        ENGINE_LOG_INFO("Mod", "清单：" + c.manifest.id + "（phase=" +
                       phaseName(c.manifest.phase) + "，依赖 " +
                       std::to_string(c.manifest.dependencies.size()) + " 个）");
        candidates.push_back(c);
    }
    return candidates;
}

// ③ 预排序：先 phase 升序，同 phase 按 id。
//    这一步只固定"同层节点"的先后，真正的前后关系由依赖排序决定。
void preSortCandidates(std::vector<Candidate>& candidates) {
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            if (a.manifest.phase != b.manifest.phase) {
                return static_cast<std::int32_t>(a.manifest.phase) <
                       static_cast<std::int32_t>(b.manifest.phase);
            }
            return a.manifest.id < b.manifest.id;
        });
}

// ④ 拓扑排序 + 打印被拒原因；返回可加载模组的下标（与 candidates 同序）。
std::vector<std::size_t> sortCandidates(const std::vector<Candidate>& candidates,
                                        std::vector<ModManifest>& items) {
    const SortResult sorted = ModSorter::sort(items);
    for (const RejectedMod& r : sorted.rejected) {
        ENGINE_LOG_ERROR("Mod", "拒绝加载 " + candidates[r.index].dirName + "：" + r.reason);
    }
    return sorted.order;
}

} // namespace

std::size_t ModLoader::loadAll(const std::string& dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        ENGINE_LOG_INFO("Mod", "模组目录不存在，按 0 个模组继续（" + dir + "）");
        return 0;
    }

    prepareModApi();

    // 收集再排序：不依赖目录枚举顺序，保证每次启动的加载次序一致
    std::vector<Candidate> candidates = collectCandidates(dir);
    preSortCandidates(candidates);
    // 排序输入：每条模组一份清单（预排序之后构造，与 candidates 同序）
    std::vector<ModManifest> items;
    items.reserve(candidates.size());
    for (const Candidate& c : candidates) {
        items.push_back(c.manifest);
    }

    const std::vector<std::size_t> order = sortCandidates(candidates, items);

    // 加载失败的模组 id 集合：被依赖方失败时，依赖方一并跳过（运行期级联）
    std::unordered_set<std::string> failedIds;

    std::size_t okCount = 0;
    for (std::size_t idx : order) {
        if (loadOne(candidates[idx].dll, candidates[idx].dirName,
                    items[idx].id, items[idx].dependencies, failedIds)) {
            ++okCount;
        }
    }

    if (okCount == 0) {
        ENGINE_LOG_INFO("Mod", "本次未加载任何模组");
    } else {
        ENGINE_LOG_INFO("Mod", "加载 " + std::to_string(okCount) + " 个模组");
    }
    return okCount;
}

// ⑤ 加载单个模组（原 loadAll 内联体的第四段）。
//    六道关卡：级联 / LoadLibrary / 导出符号 / 实例化 / 入口 ABI / onLoad；
//    每道失败都按"是否已映射、是否已实例化"分级清理，然后把 modId 记进 failedIds。
bool ModLoader::loadOne(fs::path dllPath,
                        const std::string& dirName,
                        const std::string& modId,
                        const std::vector<std::string>& dependencies,
                        std::unordered_set<std::string>& failedIds) {
    const std::string shown = dirName;

    // 依赖的模组没加载成功 → 本模组也跳过，否则它要用的东西并不存在
    for (const std::string& dep : dependencies) {
        if (failedIds.count(dep) == 0) continue;
        ENGINE_LOG_ERROR("Mod", "跳过 " + shown + "：依赖的模组未加载（" + dep + "）");
        failedIds.insert(modId);
        return false;
    }

    HMODULE h = ::LoadLibraryW(dllPath.wstring().c_str());
    if (h == nullptr) {
        ENGINE_LOG_ERROR("Mod", "LoadLibrary 失败（GetLastError=" +
                  std::to_string(static_cast<long>(::GetLastError())) + "）：" + shown);
        failedIds.insert(modId);
        return false;
    }

    // GetProcAddress 返回 FARPROC，必须先转 void* 再转目标函数指针类型：
    // 直接 FARPROC → 函数指针在 MSVC 下会触发 C4191（不安全类型转换）。
    CreateModCFn createC = reinterpret_cast<CreateModCFn>(
        reinterpret_cast<void*>(::GetProcAddress(h, kCreateModCSymbol)));

    if (createC == nullptr) {
        ENGINE_LOG_ERROR("Mod", "拒绝加载 " + shown + "：缺少 CreateModC 导出符号");
        failedIds.insert(modId);
        ::FreeLibrary(h);
        return false;
    }

    // 阶段③：实例化。构造函数里不做注册（注册一律放 onLoad，方便失败回滚）
    ModSelf self = nullptr;
    const ModVtbl* vtbl = nullptr;
    if (createC(&g_api, &self, &vtbl) != 0 || self == nullptr || vtbl == nullptr) {
        ENGINE_LOG_ERROR("Mod", "CreateModC 失败（实例或函数表为空）：" + shown);
        failedIds.insert(modId);
        ::FreeLibrary(h);
        return false;
    }
    // 入口表 ABI 必须严格相等：布局不同的话，之后每次函数表调用都会错位。
    if (vtbl->abiVersion != MODAPI_MOD_ABI_VERSION) {
        ENGINE_LOG_ERROR("Mod", "拒绝加载 " + shown + "：模组入口 ABI=" +
                         std::to_string(vtbl->abiVersion) + "，引擎要求 " +
                         std::to_string(MODAPI_MOD_ABI_VERSION));
        vtbl->destroy(self);   // 先让模组销毁实例，再卸库
        failedIds.insert(modId);
        ::FreeLibrary(h);
        return false;
    }

    // 来源标记：onLoad 里注册内容时要记到这本模组名下，必须在 onLoad 之前设置。
    // 拷成 std::string —— name(self) 返回 DLL 静态区的字符串，卸载后即失效。
    g_host.currentMod = vtbl->name(self);

    // 清单 id 与 DLL 自报名字不一致：只警告，不拒绝（排序与拒绝以清单为准）
    if (modId != vtbl->name(self)) {
        ENGINE_LOG_WARN("Mod", "清单 id 与 ModVtbl::name() 不一致：" + modId +
                         " vs " + vtbl->name(self) + "（" + shown + "）");
    }

    // 阶段④：初始化。模组在这里校验 api->version 并注册内容。
    if (vtbl->onLoad(self, &g_api) != 0) {
        ENGINE_LOG_ERROR("Mod", "onLoad 返回 false，放弃该模组：" + shown);
        // 失败回滚：把这个模组已经注册进去的内容按来源清掉，避免残留
        // （噪声表里存的是它的函数指针，随后的 FreeLibrary 会让它们悬垂）。
        const std::size_t rolledBack = engine::worldgen::NoiseRegistry::eraseByOwner(
            g_host.currentMod.c_str());
        if (rolledBack > 0) {
            ENGINE_LOG_INFO("Mod", "回滚噪声注册 " + std::to_string(rolledBack) + " 个（" + shown + "）");
        }
        vtbl->destroy(self);
        g_host.currentMod.clear();
        failedIds.insert(modId);
        ::FreeLibrary(h);
        return false;
    }

    std::ostringstream oss;
    oss << "加载成功：" << vtbl->name(self) << " v" << vtbl->version(self)
        << "（" << shown << "）";
    ENGINE_LOG_INFO("Mod", oss.str());

    // 模组资源：目录下的 assets/ 与 data/ 并入全局索引（不存在则跳过）。
    // 失败模组不登记 —— 代码没加载，其资源也不该占用名字。
    const fs::path modRoot = dllPath.parent_path();
    engine::resource::g_resourceIndex.addRoot(
        engine::resource::ResourceKind::Assets, g_host.currentMod, modRoot / "assets");
    engine::resource::g_resourceIndex.addRoot(
        engine::resource::ResourceKind::Data, g_host.currentMod, modRoot / "data");

    s_mods_.push_back(LoadedMod{dllPath.string(), g_host.currentMod, reinterpret_cast<void*>(h), self, vtbl});
    g_host.currentMod.clear();
    return true;
}

void ModLoader::unloadAll() {
    // 逆序卸载：后加载的模组可能依赖先加载的模组注册的内容，先卸它对依赖方更安全。
    for (std::size_t i = s_mods_.size(); i > 0; --i) {
        LoadedMod& m = s_mods_[i - 1U];
        if (m.self != nullptr) {
            if (m.vtbl != nullptr) {
                m.vtbl->onUnload(m.self);
                m.vtbl->destroy(m.self);   // 谁创建谁销毁：delete 在 DLL 内
            }
            m.self = nullptr;
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
            ENGINE_LOG_INFO("Mod", "已清理噪声注册 " + std::to_string(removedNoise) +
                                " 个（" + m.name + "）");
        }
        const std::size_t keptBlocks =
            engine::registry::g_blockRegistry.countByOwner(m.name.c_str());
        if (keptBlocks > 0) {
            ENGINE_LOG_INFO("Mod", "保留方块定义 " + std::to_string(keptBlocks) +
                                " 个（" + m.name + "，纯数据副本，卸载后仍可用）");
        }
        // 实体类型必须清除（与方块相反）：EntityTypeInfo 里存着 onAnimate 函数指针，
        // 指向 DLL 代码段，FreeLibrary 之后就是悬垂指针。方块定义没这个问题。
        // 顺序：先按类型销毁引用它的实例，再擦类型本身。
        {
            std::vector<std::int32_t> owned;
            const std::string& owner = m.name;
            for (auto it = engine::modhost::g_entityTypeRegistry.begin();
                 it != engine::modhost::g_entityTypeRegistry.end(); ++it) {
                if (engine::modhost::g_entityTypeRegistry.ownerOf(it->first) == owner)
                    owned.push_back(it->first);
            }
            std::size_t removedInstances = 0;
            for (std::int32_t typeId : owned) {
                removedInstances += engine::modhost::destroyEntityInstancesOfType(typeId);
                engine::modhost::g_entityTypeRegistry.erase(typeId);
            }
            if (!owned.empty()) {
                ENGINE_LOG_INFO("Mod", "已清理实体类型 " + std::to_string(owned.size())
                                    + " 个、实例 " + std::to_string(removedInstances)
                                    + " 个（" + m.name + "）");
            }
        }
        if (m.handle != nullptr) {
            ::FreeLibrary(static_cast<HMODULE>(m.handle));
            m.handle = nullptr;
        }
        ENGINE_LOG_INFO("Mod", "已卸载：" + m.path + "（" + m.name + "）");
    }
    s_mods_.clear();

    // 卸载闭环自检：噪声表与实体类型表应只剩引擎自身注册的（owner 为空），
    // 方块表按策略保留（id 已进 chunk 数据）。这是"按来源清除没有误删引擎内置内容"的直接证据。
    ENGINE_LOG_INFO("Mod", "卸载后注册表：方块 " +
                        std::to_string(engine::registry::g_blockRegistry.size()) +
                        " 个（按策略保留），噪声 " +
                        std::to_string(engine::worldgen::NoiseRegistry::size()) +
                        " 个，实体类型 " +
                        std::to_string(engine::modhost::g_entityTypeRegistry.size()) +
                        " 个（模组注册的已按来源清除）");
}

std::size_t ModLoader::loadedCount() {
    return s_mods_.size();
}

void ModLoader::attachWorld(engine::business::World* world) {
    g_host.world = world;
}

} // namespace engine::modhost

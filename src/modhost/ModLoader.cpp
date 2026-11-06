#include "modhost/ModLoader.hpp"

#include "core/Log.hpp"
#include "registry/BlockRegistry.hpp"
#include "worldgen/noise/NoiseRegistry.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <sstream>

namespace engine::modhost {
namespace {

namespace fs = std::filesystem;
struct HostContext {
    std::uint8_t nextBlockId = 7;
};

HostContext g_host;

modapi::ModApi g_api;

std::int32_t registerBlockImpl(void* host, const modapi::ModBlockDef* def) {
    if (host == nullptr || def == nullptr) return -1;
    if (def->name == nullptr || def->texture == nullptr) {
        LOG_ERROR("Mod", "registerBlock 被拒绝：name / texture 为空");
        return -1;
    }

    HostContext* ctx = static_cast<HostContext*>(host);

    if (engine::registry::g_blockRegistry.frozen()) {
        LOG_ERROR("Mod", std::string("registerBlock 被拒绝：方块注册表已冻结，加载顺序错误（")
                  + def->name + "）");
        return -1;
    }
    if (ctx->nextBlockId > 255U) {
        LOG_ERROR("Mod", "registerBlock 被拒绝：方块 id 已用尽（上限 256 个）");
        return -1;
    }

    engine::registry::BlockInfo info{};
    info.name       = def->name;
    info.opaque     = (def->opaque != 0U);
    info.hardness   = def->hardness;
    info.renderType = static_cast<engine::registry::RenderType>(def->renderType);
    info.texture    = def->texture;
    info.model      = def->model;

    const std::uint8_t id = ctx->nextBlockId;

    if (!engine::registry::g_blockRegistry.add(id, info)) {
        LOG_ERROR("Mod", std::string("registerBlock 失败：id=") +
                  std::to_string(static_cast<int>(id)) + " 已被占用（" + def->name + "）");
        return -1;
    }
    ctx->nextBlockId = static_cast<std::uint8_t>(id + 1U);

    std::ostringstream oss;
    oss << "方块注册：" << def->name << " → id=" << static_cast<int>(id)
        << "（纹理 " << def->texture << "）";
    LOG_INFO("Mod", oss.str());
    return static_cast<std::int32_t>(id);
}

// 模组日志：映射到引擎日志级别，统一前缀 [Mod]
void logImpl(std::int32_t level, const char* msg) {
    if (msg == nullptr) return;
    engine::core::LogLevel lv = engine::core::LogLevel::Info;
    if (level <= modapi::kLogDebug)      lv = engine::core::LogLevel::Debug;
    else if (level == modapi::kLogInfo)  lv = engine::core::LogLevel::Info;
    else if (level == modapi::kLogWarn)  lv = engine::core::LogLevel::Warn;
    else                                 lv = engine::core::LogLevel::Error;
    engine::core::log(lv, "Mod", msg);
}

// 注册噪声 provider：转给 worldgen 注册表。
// 与 registerBlock 同一条纪律：冻结后拒绝，避免"注册了但没人看得见"。
std::int32_t registerNoiseImpl(void*, const modapi::NoiseProviderDef* def) {
    if (def == nullptr) {
        LOG_ERROR("Mod", "registerNoise 被拒绝：def 为空");
        return -1;
    }
    const char* type = (def->type != nullptr) ? def->type : "<null>";
    if (engine::worldgen::NoiseRegistry::frozen()) {
        LOG_ERROR("Mod", std::string("registerNoise 被拒绝：噪声注册表已冻结，加载顺序错误（")
                  + type + "）");
        return -1;
    }
    if (!engine::worldgen::NoiseRegistry::registerType(*def)) {
        LOG_ERROR("Mod", std::string("registerNoise 失败：type 非法或重复（") + type + "）");
        return -1;
    }
    LOG_INFO("Mod", std::string("噪声注册：") + type);
    return 0;
}

} // namespace

std::vector<ModLoader::LoadedMod> ModLoader::s_mods_;

std::size_t ModLoader::loadAll(const std::string& dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        LOG_INFO("Mod", "模组目录不存在，按 0 个模组继续（" + dir + "）");
        return 0;
    }

    g_api = modapi::ModApi{};
    // 共用基类字段在两个子结构各填一份（继承布局：每个子结构自带 version/host/log）
    g_api.block.version       = modapi::kEngineApiVersion;
    g_api.block.host          = &g_host;
    g_api.block.log           = &logImpl;
    g_api.block.registerBlock = &registerBlockImpl;
    g_api.noise.version       = modapi::kEngineApiVersion;
    g_api.noise.host          = &g_host;
    g_api.noise.log           = &logImpl;
    g_api.noise.registerNoise = &registerNoiseImpl;

    // 先收集路径再排序：不依赖目录枚举的实现顺序，保证每次启动加载顺序一致
    // （模组之间可能有"先注册再被引用"的隐式依赖）。
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".dll") continue;
        candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end());

    std::size_t okCount = 0;
    for (const fs::path& p : candidates) {
        const std::string shown = p.filename().string();

        HMODULE h = ::LoadLibraryW(p.wstring().c_str());
        if (h == nullptr) {
            LOG_ERROR("Mod", "LoadLibrary 失败（GetLastError=" +
                      std::to_string(static_cast<long>(::GetLastError())) + "）：" + shown);
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
            ::FreeLibrary(h);
            continue;
        }

        // 阶段③：实例化。构造函数里不做注册（注册一律放 onLoad，方便失败回滚）
        modapi::IMod* mod = create(&g_api);
        if (mod == nullptr) {
            LOG_ERROR("Mod", "CreateMod 返回空指针：" + shown);
            ::FreeLibrary(h);
            continue;
        }

        // 阶段④：初始化。模组在这里校验 api->version 并注册内容。
        if (!mod->onLoad(&g_api)) {
            LOG_ERROR("Mod", "onLoad 返回 false，放弃该模组：" + shown);
            destroy(mod);
            ::FreeLibrary(h);
            continue;
        }

        std::ostringstream oss;
        oss << "加载成功：" << mod->name() << " v" << mod->version()
            << "（" << shown << "）";
        LOG_INFO("Mod", oss.str());

        s_mods_.push_back(LoadedMod{p.string(), reinterpret_cast<void*>(h), mod, destroy});
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
        if (m.handle != nullptr) {
            ::FreeLibrary(static_cast<HMODULE>(m.handle));
            m.handle = nullptr;
        }
        LOG_INFO("Mod", "已卸载：" + m.path);
    }
    s_mods_.clear();
}

std::size_t ModLoader::loadedCount() {
    return s_mods_.size();
}

} // namespace engine::modhost

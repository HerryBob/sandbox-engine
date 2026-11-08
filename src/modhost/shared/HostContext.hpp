#pragma once

#include "modapi/IMod.hpp"

#include <cstdint>
#include <string>

namespace engine::business { class World; }

namespace engine::modhost {

// 引擎侧上下文：模组调用 API 时把它的地址原样回传，引擎据此定位当前加载状态。
struct HostContext {
    // id 0 恒为空气；方块 id 从 1 起由注册顺序分配（内置 blocks 模组最先加载，占 1..6）。
    std::uint16_t nextBlockId = 1;
    // 当前正在加载的模组名（注册来源标记用）。必须是 std::string 副本：
    // mod->name() 返回的是 DLL 静态字符串，FreeLibrary 之后即悬空。
    std::string currentMod;
    // 世界查询的数据源。World 在模组加载之后才由装配层构造，未 attach 时为空。
    engine::business::World* world = nullptr;
};

// 全局唯一的加载上下文与 API 表。
extern HostContext g_host;
extern modapi::ModApi g_api;

} // namespace engine::modhost

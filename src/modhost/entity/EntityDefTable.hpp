#pragma once

#include "modhost/entity/EntityTypeInfo.hpp"
#include "registry/Registry.hpp"

#include <cstdint>
#include <string_view>

namespace engine::modhost {

// 实体类型注册表：内容全部由模组注册（引擎不再写死玩家类型）。
// 表是 inline 全局变量，模组 DLL 通过 registerEntity 回调填它，不持有副本。
inline engine::registry::Registry<EntityTypeInfo, std::int32_t> g_entityTypeRegistry;

// 封表：之后只允许读与按来源移除，禁止新增。
// 由装配层在全部模组 onLoad 之后调用（与方块表同窗口）。
inline void freezeEntityTypeRegistry() {
    g_entityTypeRegistry.freeze();
}

// 按实体类型名查类型 id；未找到返回 -1（合法 id 从 0 起）。
inline std::int32_t findEntityTypeId(std::string_view name) {
    for (auto it = g_entityTypeRegistry.begin(); it != g_entityTypeRegistry.end(); ++it) {
        if (std::string_view(it->second.name) == name) {
            return it->first;
        }
    }
    return -1;
}

} // namespace engine::modhost

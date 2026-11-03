#pragma once

#include "registry/Registry.hpp"
#include "registry/BlockInfo.hpp"
#include "shared/block/Block.hpp"

#include <string_view>

namespace engine::registry {
    inline Registry<BlockInfo> g_blockRegistry;

    inline void initBlockRegistry() {
        g_blockRegistry.add(
            static_cast<std::uint8_t>(engine::shared::BlockType::Stone),
            { "stone", true, 1.5f, RenderType::Solid, "stone" });
        g_blockRegistry.add(
            static_cast<std::uint8_t>(engine::shared::BlockType::Wood),
            { "wood", true, 2.0f, RenderType::Solid, "log_side", "wood" });  // 模型化：顶/底 log_top，侧 log_side
        g_blockRegistry.add(
            static_cast<std::uint8_t>(engine::shared::BlockType::Dirt),
            { "dirt", true, 0.5f, RenderType::Solid, "dirt" });
        g_blockRegistry.add(
            static_cast<std::uint8_t>(engine::shared::BlockType::Brick),
            { "brick", true, 2.0f, RenderType::Solid, "brick" });
        g_blockRegistry.add(
           static_cast<std::uint8_t>(engine::shared::BlockType::Glass),
            {"glass", false, 0.5f, RenderType::Cutout, "glass"});
        g_blockRegistry.add(
            static_cast<std::uint8_t>(engine::shared::BlockType::Grass),
            { "grass", true, 0.6f, RenderType::Solid, "grass_top", "grass" });  // 模型化：顶 grass_top，侧 grass_side，底 dirt
    }

    // 锁表：freeze 之后 Registry::add 一律返回 false（免锁读的前提）。
    // 注意：freeze 不在 initBlockRegistry 内部调用，而是由装配层在"内置方块 + 模组方块"
    // 全部注册完之后显式调用 —— 否则模组没有任何机会注册（#41 加载器）。
    inline void freezeBlockRegistry() {
        g_blockRegistry.freeze();
    }

    // 按方块名查 BlockType id（数据文件用字符串引用方块：strata 的 "grass"）。
    // 未找到返回 0xFF（Air=0 是合法值，故用 0xFF 作哨兵）。
    inline std::uint8_t findBlockIdByName(std::string_view name) {
        for (auto it = g_blockRegistry.begin(); it != g_blockRegistry.end(); ++it) {
            const BlockInfo& info = it->second;
            if (info.name && std::string_view(info.name) == name) {
                return static_cast<std::uint8_t>(it->first);
            }
        }
        return 0xFF;
    }
}
#pragma once

#include "registry/Registry.hpp"
#include "registry/BlockInfo.hpp"
#include "shared/block/Block.hpp"

namespace engine::registry {
    inline Registry<BlockInfo> g_blockRegistry;

    inline void initBlockRegistry() {
        g_blockRegistry.add(
            static_cast<std::uint8_t>(engine::shared::BlockType::Stone),
            { "stone", true, 1.5f, RenderType::Solid });
        g_blockRegistry.add(
            static_cast<std::uint8_t>(engine::shared::BlockType::Wood),
            { "wood", true, 2.0f, RenderType::Solid });
        g_blockRegistry.add(
            static_cast<std::uint8_t>(engine::shared::BlockType::Dirt),
            { "dirt", true, 0.5f, RenderType::Solid });
        g_blockRegistry.add(
            static_cast<std::uint8_t>(engine::shared::BlockType::Brick),
            { "brick", true, 2.0f, RenderType::Solid });
        g_blockRegistry.add(
           static_cast<std::uint8_t>(engine::shared::BlockType::Glass),
            {"glass", false, 0.5f, RenderType::Cutout});
        g_blockRegistry.freeze();   // 锁表（免锁读前提）
    }
}
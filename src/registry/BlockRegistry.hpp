#pragma once

#include "core/Log.hpp"
#include "registry/Registry.hpp"
#include "registry/BlockInfo.hpp"
#include "shared/block/Block.hpp"

#include <cstdint>
#include <string_view>

namespace engine::registry {
    // 方块注册表：内容全部由 mods/blocks.dll 注册（#39 之后引擎不再写死任何方块）。
    // 表本身是 inline 全局变量，模组 DLL 通过 registerBlock 回调填它，不持有副本。
    inline Registry<BlockInfo> g_blockRegistry;

    // 锁表：freeze 之后 Registry::add 一律返回 false（免锁读的前提）。
    // 由装配层在"内置 blocks 模组 + 其他模组"全部注册完之后显式调用 ——
    // 提前 freeze 不会报错，只会静默少方块。
    inline void freezeBlockRegistry() {
        g_blockRegistry.freeze();
    }

    // 按方块名查 id（数据文件用字符串引用方块：strata 的 "grass"）。
    // 未找到返回 0xFFFF（Air=0 是合法值，故用 0xFFFF 作哨兵）。
    inline std::uint16_t findBlockIdByName(std::string_view name) {
        for (auto it = g_blockRegistry.begin(); it != g_blockRegistry.end(); ++it) {
            const BlockInfo& info = it->second;
            if (std::string_view(info.name) == name) {
                return static_cast<std::uint16_t>(it->first);
            }
        }
        return 0xFFFF;
    }

    // 内置方块 id：id 由注册顺序决定，引擎不能再假设固定值，
    // 必须在 freeze 之后按名字解析一次，之后统一读这张缓存。
    // 0xFFFF = 未解析/缺失。
    struct BuiltinBlockIds {
        std::uint16_t stone = 0xFFFF;
        std::uint16_t wood  = 0xFFFF;
        std::uint16_t dirt  = 0xFFFF;
        std::uint16_t brick = 0xFFFF;
        std::uint16_t glass = 0xFFFF;
        std::uint16_t grass = 0xFFFF;
    };

    inline BuiltinBlockIds g_builtinBlockIds;

    // 解析内置方块 id；缺失项保持 0xFFFF 并打 Warn（多半是 mods/blocks.dll 没加载）。
    inline void resolveBuiltinBlockIds() {
        g_builtinBlockIds.stone = findBlockIdByName("stone");
        g_builtinBlockIds.wood  = findBlockIdByName("wood");
        g_builtinBlockIds.dirt  = findBlockIdByName("dirt");
        g_builtinBlockIds.brick = findBlockIdByName("brick");
        g_builtinBlockIds.glass = findBlockIdByName("glass");
        g_builtinBlockIds.grass = findBlockIdByName("grass");

        if (g_builtinBlockIds.stone == 0xFFFF || g_builtinBlockIds.dirt == 0xFFFF ||
            g_builtinBlockIds.grass == 0xFFFF) {
            LOG_WARN("Block", "内置方块缺失（mods/blocks.dll 未加载？）——世界生成将没有地表");
        }
    }
}
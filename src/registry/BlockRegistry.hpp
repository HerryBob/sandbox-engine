#pragma once

#include "core/Log.hpp"
#include "registry/Registry.hpp"
#include "registry/BlockInfo.hpp"
#include "shared/block/Block.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
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

    // B67：哨兵值集中一处 —— 原来 0xFFFF 散落在 12 个地方，改 key 宽度时必然漏改。
    // 意义：key 类型的最大值（Air = 0 是合法 id，所以不能用 0 当"未找到"）。
    inline constexpr std::uint16_t kInvalidBlockId =
        (std::numeric_limits<std::uint16_t>::max)();

    // 按方块名查 id（数据文件用字符串引用方块：strata 的 "grass"）。
    // 未找到返回 kInvalidBlockId。
    // B66（已评估，本轮不改）：O(N) 线性扫描。调用点只有启动期的 resolveBuiltinBlockIds
    // 与少数模组注册路径（方块总数 < 100），加 name→id 索引属于为不存在的热点优化。
    inline std::uint16_t findBlockIdByName(std::string_view name) {
        for (auto it = g_blockRegistry.begin(); it != g_blockRegistry.end(); ++it) {
            const BlockInfo& info = it->second;
            if (std::string_view(info.name) == name) {
                return static_cast<std::uint16_t>(it->first);
            }
        }
        return kInvalidBlockId;
    }

    // 内置方块 id：id 由注册顺序决定，引擎不能再假设固定值，
    // 必须在 freeze 之后按名字解析一次，之后统一读这张缓存。
    // kInvalidBlockId = 未解析/缺失。
    struct BuiltinBlockIds {
        std::uint16_t stone = kInvalidBlockId;
        std::uint16_t wood  = kInvalidBlockId;
        std::uint16_t dirt  = kInvalidBlockId;
        std::uint16_t brick = kInvalidBlockId;
        std::uint16_t glass = kInvalidBlockId;
        std::uint16_t grass = kInvalidBlockId;
    };

    inline BuiltinBlockIds g_builtinBlockIds;

    // 解析内置方块 id；缺失项保持 kInvalidBlockId 并打 Warn（多半是 mods/blocks.dll 没加载）。
    // B68：**必须在 freezeBlockRegistry() 之后调用** —— 封表前注册表可能还是空的，
    // 此时解析会把所有项都留成"缺失"，而且不会有任何提示（这里加断言把顺序错误暴露）。
    // B69：警告只检查 stone/dirt/grass 三项，因为生成器只硬依赖这三个（地表/次表层）；
    //      其它三项缺失只会影响玩家放置（右键固定放 stone → 缺 stone 时放置会失败）。
    inline void resolveBuiltinBlockIds() {
        assert(g_blockRegistry.frozen() &&
               "resolveBuiltinBlockIds 必须在 freezeBlockRegistry() 之后调用");

        g_builtinBlockIds.stone = findBlockIdByName("stone");
        g_builtinBlockIds.wood  = findBlockIdByName("wood");
        g_builtinBlockIds.dirt  = findBlockIdByName("dirt");
        g_builtinBlockIds.brick = findBlockIdByName("brick");
        g_builtinBlockIds.glass = findBlockIdByName("glass");
        g_builtinBlockIds.grass = findBlockIdByName("grass");

        if (g_builtinBlockIds.stone == kInvalidBlockId ||
            g_builtinBlockIds.dirt  == kInvalidBlockId ||
            g_builtinBlockIds.grass == kInvalidBlockId) {
            ENGINE_LOG_WARN("Block", "内置方块缺失（mods/blocks.dll 未加载？）——世界生成将没有地表");
        }
    }
}
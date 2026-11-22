#pragma once

#include <stddef.h>   // offsetof（C 与 C++ 都提供）

#include "modapi/block_api.h"
#include "modapi/entity_api.h"
#include "modapi/noise_api.h"
#include "modapi/world_api.h"

// 引擎注入给模组的全量能力包：按需取用子结构。
//   需要方块 → api->block.*    需要噪声 → api->noise.*
//   需要世界 → api->world.*    需要实体 → api->entity.*
// 扩展纪律：新增能力 = 新子结构追加在末尾（老模组访问前面子结构的偏移不变）。
typedef struct ModApi {
    BlockApi  block;    // 方块注册能力
    NoiseApi  noise;    // 噪声注册能力
    WorldApi  world;    // 世界查询能力（v4 追加）
    EntityApi entity;   // 实体类型注册能力（v5 追加）
} ModApi;

// ============================================================================
// 布局锁（仅 C++ 编译期的额外检查，不参与 API）
// ----------------------------------------------------------------------------
// 跨 DLL 传递的结构体，偏移就是契约。任何一次"改字段顺序/插字段/换类型"
// 都会在这里编译失败 —— 那时必须同时：① 升 kEngineApiVersion ② 通知模组重编。
// 这些数字也是 C 侧手写展开（BlockApi 开头三字段等）的正确性证明。
// ============================================================================
#ifdef __cplusplus
static_assert(offsetof(ApiBase, version) == 0,  "ApiBase.version 偏移变了");
static_assert(offsetof(ApiBase, host)    == 8,  "ApiBase.host 偏移变了");
static_assert(offsetof(ApiBase, log)     == 16, "ApiBase.log 偏移变了");
static_assert(sizeof(ApiBase)            == 24, "ApiBase 大小变了");

static_assert(offsetof(ModBlockDef, name)       == 0,  "ModBlockDef.name 偏移变了");
static_assert(offsetof(ModBlockDef, texture)    == 24, "ModBlockDef.texture 偏移变了");
static_assert(offsetof(ModBlockDef, model)      == 32, "ModBlockDef.model 偏移变了");
static_assert(offsetof(ModBlockDef, mass)       == 40, "ModBlockDef.mass 偏移变了（v3 追加字段必须留在末尾）");
static_assert(offsetof(ModBlockDef, friction)   == 48, "ModBlockDef.friction 偏移变了");
static_assert(sizeof(ModBlockDef)               == 56, "ModBlockDef 大小变了（老模组会越界读）");

static_assert(offsetof(BlockApi,  registerBlock)      == 24, "BlockApi 能力字段偏移变了");
static_assert(sizeof(BlockApi)                        == 32, "BlockApi 大小变了");
static_assert(offsetof(NoiseApi,  registerNoise)      == 24, "NoiseApi 能力字段偏移变了");
static_assert(sizeof(NoiseApi)                        == 32, "NoiseApi 大小变了");
static_assert(offsetof(WorldApi,  getBlock)           == 24, "WorldApi.getBlock 偏移变了");
static_assert(offsetof(WorldApi,  forEachBlockInAABB) == 32, "WorldApi.forEachBlockInAABB 偏移变了");
static_assert(offsetof(WorldApi,  getWorldInfo)       == 40, "WorldApi.getWorldInfo 偏移变了");
static_assert(sizeof(WorldApi)                        == 48, "WorldApi 大小变了");
static_assert(offsetof(EntityApi, registerEntity)     == 24, "EntityApi 能力字段偏移变了");
static_assert(sizeof(EntityApi)                       == 32, "EntityApi 大小变了");

static_assert(offsetof(ModApi, block)  == 0,   "ModApi.block 偏移变了");
static_assert(offsetof(ModApi, noise)  == 32,  "ModApi.noise 偏移变了");
static_assert(offsetof(ModApi, world)  == 64,  "ModApi.world 偏移变了");
static_assert(offsetof(ModApi, entity) == 112, "ModApi.entity 偏移变了（v5 追加项必须留在末尾）");
static_assert(sizeof(ModApi)           == 144, "ModApi 大小变了（老模组会越界读）");
#endif

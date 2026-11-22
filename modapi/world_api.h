#pragma once

#include "modapi/api_base.h"

// 世界查询返回值：负数一律是错误，非负是查询结果。
static const int32_t kWorldOk           =  0;
static const int32_t kWorldNotReady     = -1;  // World 尚未创建（onLoad 阶段必然如此）
static const int32_t kWorldAabbTooLarge = -2;  // 区域体积超过 kMaxAabbVolume
static const int32_t kWorldBadArgs      = -3;  // 空指针，或 min 与 max 顺序颠倒

// getBlock 的格子状态（已加载前提下才有意义）
static const int32_t kBlockNotLoaded = 0;  // 所在 chunk 未加载
static const int32_t kBlockAir       = 1;  // 已加载，该格是空气
static const int32_t kBlockOccupied  = 2;  // 已加载，该格有方块（outId 有效）

// 单次 AABB 查询的格数上限（32³）。超过由模组自己切成多次调用，
// 避免一次查询把整片世界拷进临时表。
static const int32_t kMaxAabbVolume = 32768;

// 世界坐标：与引擎的 BlockPos 同宽（int32），避免跨 DLL 传可变宽度类型。
typedef struct ModBlockPos {
    int32_t x;
    int32_t y;
    int32_t z;
} ModBlockPos;

// 世界概况；字段口径见各自注释。
typedef struct ModWorldInfo {
    uint32_t loadedChunks;  // 已加载的非空 chunk 数（全空气 chunk 不占槽位）
    uint64_t totalBlocks;   // 已加载 chunk 内的非空气方块总数
} ModWorldInfo;

// 遍历回调：引擎在锁外逐条调用，回调里可以安全地再调 getBlock / getWorldInfo。
// user：模组原样传入的上下文指针，引擎不解释。
typedef void (*ModBlockVisitor)(void* user, const ModBlockPos* pos, uint16_t blockId);

// 世界查询能力（只读：修改世界不在本结构里）。
// C 没有继承：开头三字段是 ApiBase 的手写展开。
typedef struct WorldApi {
    // ---- ApiBase 展开 ----
    uint32_t version;
    void*    host;
    void   (*log)(int32_t level, const char* msg);
    // ---- 世界能力 ----
    // 查单个格子。返回值：kBlockNotLoaded / kBlockAir / kBlockOccupied，或 kWorldNotReady 等错误。
    // outId 仅在返回 kBlockOccupied 时写入，其余情况不碰。
    int32_t (*getBlock)(void* host, const ModBlockPos* pos, uint16_t* outId);

    // 遍历 [min, max] 闭区间内的所有非空气方块（未加载区域自动跳过）。
    // 返回 kWorldOk / kWorldNotReady / kWorldAabbTooLarge / kWorldBadArgs。
    int32_t (*forEachBlockInAABB)(void* host,
                                  const ModBlockPos* min,
                                  const ModBlockPos* max,
                                  ModBlockVisitor visitor,
                                  void* user);

    // 读世界概况。返回 kWorldOk / kWorldNotReady / kWorldBadArgs。
    int32_t (*getWorldInfo)(void* host, ModWorldInfo* out);
} WorldApi;

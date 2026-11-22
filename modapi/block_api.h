#pragma once

#include "modapi/api_base.h"

// 渲染管线分桶（值只增不改：新引擎加新值，老模组不认识也不受影响）
static const uint8_t kRenderSolid       = 0;
static const uint8_t kRenderCutout      = 1;
static const uint8_t kRenderTranslucent = 2;

// 方块注册描述：纯 POD，固定布局。
typedef struct ModBlockDef {
    const char*  name;        // 方块名（数据文件 strata 里用这个字符串引用它）
    uint8_t      opaque;      // 1 = 不透明（参与面剔除）；0 = 镂空/半透明
    float        hardness;    // 挖掘硬度（业务层用）
    uint8_t      renderType;  // kRenderSolid / kRenderCutout / kRenderTranslucent
    const char*  texture;     // 纹理图集里的纹理名（assets/textures/block/<name>.png）
    const char*  model;       // 模型名；nullptr = 默认立方体（6 面用 texture）

    // ---- v3 追加：物理属性（字段一律加在末尾，保持 ABI 追加纪律）----
    float        mass;        // 质量（相对单位：1.0 = 一个基准方块，约 1 m³ 的土）
    uint8_t      solid;       // 1 = 参与碰撞（实体阻挡、可站立）；0 = 可穿过
    float        friction;    // 摩擦系数（0 = 冰面，1 = 完全不滑）
} ModBlockDef;

// 方块注册能力。
// C 没有继承：开头三字段是 ApiBase 的手写展开（顺序/类型必须与 api_base.h 一致）。
// 模组需要方块能力时用 api->block.*；日志/版本/上下文就是开头那三个字段。
typedef struct BlockApi {
    // ---- ApiBase 展开 ----
    uint32_t version;
    void*    host;
    void   (*log)(int32_t level, const char* msg);
    // ---- 方块能力 ----
    // 注册一个方块：成功返回分配的方块 id（>= 0）；失败返回 -1。
    // 由引擎实现（registerBlockImpl），模组调用 —— 调用方向：模组 → 引擎。
    int32_t (*registerBlock)(void* host, const ModBlockDef* def);
} BlockApi;

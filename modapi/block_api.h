#pragma once

#include "modapi/api_base.h"

namespace modapi {

inline constexpr std::uint8_t kRenderSolid       = 0;
inline constexpr std::uint8_t kRenderCutout      = 1;
inline constexpr std::uint8_t kRenderTranslucent = 2;

// 方块注册描述：纯 POD，固定布局。
struct ModBlockDef {
    const char*  name;        // 方块名（数据文件 strata 里用这个字符串引用它）
    std::uint8_t opaque;      // 1 = 不透明（参与面剔除）；0 = 镂空/半透明
    float        hardness;    // 挖掘硬度（业务层用）
    std::uint8_t renderType;  // kRenderSolid / kRenderCutout / kRenderTranslucent
    const char*  texture;     // 纹理图集里的纹理名（assets/textures/block/<name>.png）
    const char*  model;       // 模型名；nullptr = 默认立方体（6 面用 texture）
};

// 方块注册能力：继承共用基类。
// 模组需要方块能力时用 api->block.*；日志/版本/上下文是基类字段（block 自带一份）。
struct BlockApi : ApiBase {
    // 注册一个方块：成功返回分配的方块 id（>= 0）；失败返回 -1。
    // 由引擎实现（registerBlockImpl），模组调用 —— 调用方向：模组 → 引擎。
    std::int32_t (*registerBlock)(void* host, const ModBlockDef* def);
};

} // namespace modapi

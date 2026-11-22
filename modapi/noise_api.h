#pragma once

#include "modapi/api_base.h"

// str 非空 → 用 str（如 fbm 的 base 类型名）；str 为空 → 用 num。
typedef struct NoiseParamC {
    const char* key;
    const char* str;
    double      num;
} NoiseParamC;

// 创建实例：成功返回不透明句柄（非空）；失败返回 nullptr，
// 可选通过 errOut 回传静态字符串说明原因（DLL 内的常量，生命周期同 DLL）。
// 由模组实现，引擎在 make 时调用 —— 调用方向：引擎 → 模组。
typedef void* (*NoiseCreateFn)(const NoiseParamC* params, int32_t count,
                               const char** errOut);

// 采样：2D 高度图，世界坐标未缩放，seed 显式传入。
// 由模组实现，引擎逐列调用（热路径）。
typedef double (*NoiseSampleFn)(void* self, double x, double z, uint64_t seed);

// 销毁：DLL 内释放 create 分配的实例（谁创建谁销毁）。
// 由模组实现，引擎卸载时调用。
typedef void (*NoiseDestroyFn)(void* self);

// 一个 provider 的完整描述。引擎按 type 存进注册表，make 时调 create。
typedef struct NoiseProviderDef {
    const char*     type;     // 注册名（JSON 里的 "type" 字符串）
    NoiseCreateFn   create;
    NoiseSampleFn   sample;
    NoiseDestroyFn  destroy;
} NoiseProviderDef;

// 噪声注册能力。
// C 没有继承：开头三字段是 ApiBase 的手写展开。
typedef struct NoiseApi {
    // ---- ApiBase 展开 ----
    uint32_t version;
    void*    host;
    void   (*log)(int32_t level, const char* msg);
    // ---- 噪声能力 ----
    // 注册一个噪声 provider：成功返回 0；失败返回 -1（type 非法或重复）。
    // 由引擎实现（registerNoiseImpl），模组调用 —— 调用方向：模组 → 引擎。
    int32_t (*registerNoise)(void* host, const NoiseProviderDef* def);
} NoiseApi;

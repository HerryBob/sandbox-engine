#pragma once

// ============================================================================
// 模组入口契约（纯 C，可被任何语言实现）
// ============================================================================
// 这是引擎与模组之间**唯一**的二进制接口约定。它只包含：
//   ① 一张函数表（ModVtbl）：模组导出，引擎只读后逐项调用
//   ② 一个导出入口（CreateModC）：引擎用它拿到"实例 + 函数表"
//
// 为什么是 C：跨 DLL 只允许"定长标量 + 函数指针"，不能出现 C++ 虚表/STL 类型，
// 否则编译器的虚表布局、异常模型、STL 布局都会成为隐式 ABI 依赖。
//
// 与 ModApi（mod_api.h）的分工：
//   ModVtbl  —— "怎么进入模组"（本文件，几乎不变）
//   ModApi   —— "模组能调哪些引擎能力"（会随版本追加，含 kEngineApiVersion）
// 两者版本号独立演进。

#include <stdint.h>

// 入口表 ABI 版本：ModVtbl 的字段顺序/增减变化时才 +1。
// 模组必须把它编译进 vtbl->abiVersion，引擎加载时严格比对。
#define MODAPI_MOD_ABI_VERSION 1

// 导出符号名（引擎 GetProcAddress 用）
static const char* const kCreateModCSymbol = "CreateModC";

// 不透明实例句柄：引擎只存不解释、原样传回。
typedef void* ModSelf;

// 模组函数表。所有函数第一个参数都是实例（等价于 C++ 的 this）。
// 返回值约定：onLoad 返回 0 = 成功，非 0 = 失败（引擎放弃该模组）。
typedef struct ModVtbl {
    uint32_t    abiVersion;                      // 必须等于 MODAPI_MOD_ABI_VERSION
    const char* (*name)(ModSelf self);           // 模组名（引擎会立刻拷贝，不留指针）
    const char* (*version)(ModSelf self);        // 模组版本（日志用）
    int32_t     (*onLoad)(ModSelf self, const void* api);   // api 实际是 const ModApi*
    void        (*onUnload)(ModSelf self);       // 卸载前清理（不得抛异常）
    void        (*destroy)(ModSelf self);        // 释放实例：谁 new 谁 delete，必须在模组内
} ModVtbl;

// 每个模组 DLL 必须导出的唯一入口（extern "C"，名字不修饰）。
// 成功：返回 0，写出实例与函数表；失败：返回非 0，引擎不加载。
// api 在 Create 阶段就传入（便于构造期打日志），但**注册内容仍必须留到 onLoad**，
// 否则 onLoad 失败时无法干净回滚。
typedef int32_t (*CreateModCFn)(const void* api, ModSelf* outSelf, const ModVtbl** outVtbl);

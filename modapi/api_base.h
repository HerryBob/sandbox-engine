#pragma once

// 本文件（以及 modapi/ 下**所有**头）都是纯 C 的 —— 2026-11-22 的 C 接口改造后，
// 原来的 C++ 模组基类头 IMod.hpp 已删除，modapi/ 里不再有 C++ 头。
// 只用语言无关的构造：定长标量、指针、函数指针、struct、typedef、静态常量。
// 禁止出现：namespace、模板、类、继承、异常、STL 类型、引用。
//
// C++ 模组同样能 include 这些头（C 头在 C++ 中合法），只是类型在全局作用域，
// 写法是 ModApi / ModBlockDef 而不是 modapi::ModApi。

#include <stdint.h>

// 引擎 API 版本：v5（v5 = ModApi 末尾追加 EntityApi，实体类型注册）。
// 契约演进只允许在结构末尾追加字段（ABI 纪律），
// 老模组若未重编，version 校验会拒绝加载，不会带着错位布局继续跑。
// 用 static const 而不是 constexpr：C 合法，且类型明确（避免 enum 隐式变成 int）。
static const uint32_t kEngineApiVersion = 5u;

// 日志级别（值只增不改，模组可直接比较大小）
static const int32_t kLogDebug = 0;
static const int32_t kLogInfo  = 1;
static const int32_t kLogWarn  = 2;
static const int32_t kLogError = 3;

// 共用基类字段：每个能力结构都带一份（版本 / 引擎上下文 / 日志）。
//
// C 没有继承，所以各 XxxApi 结构把这三个字段**手写展开在最前面**，
// 布局与 C++ 的"struct BlockApi : ApiBase"完全一致（标准布局单继承 =
// 基类子对象在前 + 派生字段在后，且 ApiBase 无尾部填充）。
// ApiBase 本身保留下来，作为"展开顺序与偏移"的基准和文档。
typedef struct ApiBase {
    uint32_t version;                                   // 引擎 API 版本（模组第一件事校验它）
    void*    host;                                      // 引擎侧上下文，模组不认识、原样传回
    void   (*log)(int32_t level, const char* msg);      // 统一日志（引擎实现，模组调用）
} ApiBase;

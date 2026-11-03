#pragma once
// ============================================================================
// modapi —— 模组与引擎之间唯一的契约层
// ============================================================================
// 为什么要有这一层：
//   模组是独立编译的 DLL，不能链接引擎 exe 的符号（Windows 下 exe 默认不导出
//   任何符号，且 C++ 名字修饰 / 运行库 ABI 都不稳定）。所以引擎在 CreateMod()
//   时把一张"函数指针表"（EngineApi）交给模组，模组通过它回调引擎能力。
//
// 三条硬约束（违反会静默失效，不报错，最难查）：
//   1. 本目录禁止 include 任何引擎头文件。
//      BlockRegistry.hpp 里的 g_blockRegistry 是 inline 全局变量：
//      DLL 一旦 include 它，会在 DLL 自己的地址空间里生成一份独立副本，
//      模组往副本里注册，引擎完全看不见 —— 表现就是"日志说注册成功，世界里没有"。
//   2. 跨边界只传定长类型（uint8_t / int32_t / float / const char*），
//      不传 std::string、std::vector、模板类 —— 不同编译器/运行库下布局无保证。
//   3. 字符串由模组提供，必须保证与 DLL 同生命周期（用静态字符串或 DLL 内常量，
//      不要传栈上临时 std::string 的 c_str()）。
//
// 版本纪律：任何字段增删或语义改变，kEngineApiVersion 必须 +1。
//          引擎与模组版本不一致时拒绝加载，而不是带着错位的内存布局继续跑。
// ============================================================================

#include <cstdint>

namespace modapi {

// 接口版本号（当前 1）。引擎侧和模组侧必须一致。
inline constexpr std::uint32_t kEngineApiVersion = 1;

// 日志级别（数值与引擎 core::LogLevel 对齐，方便直接映射）
inline constexpr std::int32_t kLogDebug = 0;
inline constexpr std::int32_t kLogInfo  = 1;
inline constexpr std::int32_t kLogWarn  = 2;
inline constexpr std::int32_t kLogError = 3;

// 渲染分桶（数值与引擎 registry::RenderType 对齐）
inline constexpr std::uint8_t kRenderSolid       = 0;
inline constexpr std::uint8_t kRenderCutout      = 1;
inline constexpr std::uint8_t kRenderTranslucent = 2;

// 方块注册描述：纯 POD，固定布局。
// 字段含义与引擎 registry::BlockInfo 一一对应，但这里刻意不 include 它。
struct ModBlockDef {
    const char*  name;        // 方块名（数据文件 strata 里用这个字符串引用它）
    std::uint8_t opaque;      // 1 = 不透明（参与面剔除）；0 = 镂空/半透明
    float        hardness;    // 挖掘硬度（业务层用）
    std::uint8_t renderType;  // kRenderSolid / kRenderCutout / kRenderTranslucent
    const char*  texture;     // 纹理图集里的纹理名（assets/textures/block/<name>.png）
    const char*  model;       // 模型名；nullptr = 默认立方体（6 面用 texture）
};

// 引擎传给模组的函数指针表。
// 由模组在 onLoad 期间使用；onLoad 返回后仍可使用（host 指针常驻到引擎退出）。
struct EngineApi {
    std::uint32_t version;  // 恒等于 kEngineApiVersion（引擎写入，模组校验）

    // 引擎侧上下文，模组不要解引用，原样透传给下面两个回调即可。
    void* host;

    // 注册方块：成功返回引擎分配的 id（≥0），失败返回 -1。
    // id 由引擎统一分配，模组不能自己指定 —— 否则两个模组撞号会静默覆盖。
    std::int32_t (*registerBlock)(void* host, const ModBlockDef* def);

    // 打日志：走引擎的统一日志锁与前缀，避免多线程输出交错。
    void (*log)(std::int32_t level, const char* msg);
};

} // namespace modapi

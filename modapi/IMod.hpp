#pragma once
// ============================================================================
// modapi/IMod.hpp —— 模组生命周期接口 + DLL 导出函数约定
// ============================================================================
// 一个模组 DLL 必须导出两个 C 链接函数（不带参数修饰名，host 用 GetProcAddress 找）：
//
//   extern "C" __declspec(dllexport) modapi::IMod* CreateMod(const modapi::EngineApi*);
//   extern "C" __declspec(dllexport) void DestroyMod(modapi::IMod*);
//
// 内存归属：IMod 由模组 DLL 自己 new、由模组 DLL 自己 delete（DestroyMod 内）。
//           引擎只持有指针，绝不 delete —— 跨运行库释放堆内存会直接崩。
// ============================================================================

#include "modapi/ModApi.hpp"

namespace modapi {

// 模组实例接口。
// 注意：这是跨 DLL 边界的 C++ 虚接口，依赖"两边用同一个编译器、同一套虚表布局"。
// 所以虚函数顺序一旦发布就不可改动；要扩展请新增接口（IModV2）而不是插函数。
class IMod {
public:
    virtual ~IMod() {}

    // 模组名（用于日志与去重），必须返回 DLL 内常驻字符串。
    virtual const char* name() const = 0;

    // 模组版本（字符串形式，如 "0.1"）。
    virtual const char* version() const = 0;

    // 加载阶段：注册内容（方块、未来的噪声/预设等）。
    // api 保证非空且 version 已由引擎写入，模组必须自己校验版本。
    // 返回 false = 放弃本模组（引擎会调用 DestroyMod 后卸载 DLL）。
    virtual bool onLoad(const EngineApi* api) = 0;

    // 卸载阶段：释放模组自己持有的资源。
    // 不要在这里反向调用引擎注册表删除 —— 注册表是引擎的，随引擎退出一起消失。
    virtual void onUnload() = 0;
};

// 导出函数的函数指针类型（host 侧 GetProcAddress 后转成它来调用）
using CreateModFn  = IMod* (*)(const EngineApi* api);
using DestroyModFn = void  (*)(IMod* mod);

// 导出符号名（写成常量，避免两处手写字符串写错）
inline constexpr const char* kCreateModSymbol  = "CreateMod";
inline constexpr const char* kDestroyModSymbol = "DestroyMod";

} // namespace modapi

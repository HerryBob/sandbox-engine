// ============================================================================
// 示例模组：hello_block
// ============================================================================
// 目的：验证"模组 DLL → EngineApi 函数指针 → 引擎方块注册表"这条跨边界链路。
// 它注册一个 mod_brick 方块（复用内置的 brick 纹理），用来证明：
//   ① DLL 里的代码能回调进引擎（而不是写进 DLL 自己的注册表副本）
//   ② 模组注册的方块能被世界生成（strata 里写 "mod_brick" 就能用）
//
// 编译产物：build/Debug/mods/hello_block.dll（由 CMake 的 hello_block target 负责）
// 依赖纪律：只 include modapi/ 两个头文件，绝不 include src/ 下任何引擎头文件。
// ============================================================================

#include "modapi/IMod.hpp"

#include <cstdint>
#include <new>   // std::nothrow

namespace {

class HelloBlockMod : public modapi::IMod {
public:
    const char* name() const override { return "hello_block"; }
    const char* version() const override { return "0.1"; }

    bool onLoad(const modapi::EngineApi* api) override {
        if (api == nullptr) return false;

        // 版本校验：接口一旦改版就拒绝加载，而不是带着错位布局继续跑。
        if (api->version != modapi::kEngineApiVersion) {
            api->log(modapi::kLogError, "接口版本不匹配，拒绝加载");
            return false;
        }

        // 全部字段显式赋值（这个结构是跨边界 POD，不留未初始化字节）。
        modapi::ModBlockDef def{};
        def.name       = "mod_brick";
        def.opaque     = 1U;
        def.hardness   = 2.0f;
        def.renderType = modapi::kRenderSolid;
        // 复用内置 brick 纹理：图集里已有这个名字，不需要额外准备 PNG
        def.texture    = "brick";
        def.model      = nullptr;

        const std::int32_t id = api->registerBlock(api->host, &def);
        if (id < 0) {
            api->log(modapi::kLogError, "注册 mod_brick 失败（引擎侧已打详细原因）");
            return false;
        }

        api->log(modapi::kLogInfo, "hello_block 已注册方块 mod_brick");
        return true;
    }

    void onUnload() override {
        // 本模组没有自己持有的堆资源（方块注册表归引擎管），无需清理。
    }
};

} // namespace

// ---- DLL 导出入口（C 链接，名字不修饰，host 用 GetProcAddress 精确查找）----

extern "C" __declspec(dllexport) modapi::IMod* CreateMod(const modapi::EngineApi* api) {
    // 用堆对象而不是静态对象：多实例/热重载时更安全，且销毁路径统一走 DestroyMod。
    return new (std::nothrow) HelloBlockMod();
}

extern "C" __declspec(dllexport) void DestroyMod(modapi::IMod* mod) {
    // 谁 new 谁 delete：这个 delete 必须发生在本 DLL 内，否则跨运行库释放会崩。
    delete mod;
}

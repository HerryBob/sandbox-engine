// ============================================================================
// 内置方块模组：builtin_blocks
// ============================================================================
// #39：把原先写死在引擎 initBlockRegistry() 里的 6 个方块搬到这里。
// 这是"引擎零内置内容"的验收件 —— 删掉 mods/blocks/ 整个目录，方块注册表就是空的。
//
// 顺序即 id：注册表从 1 开始按注册顺序分配，本文件严格按旧版枚举顺序注册
// （stone / wood / dirt / brick / glass / grass），因此 id 仍是 1..6，旧世界数据不会错位。
// 加载次序由 mods/blocks/mod.json 的 phase=builtin 保证（先于 normal 批）。
//
// 依赖纪律：只 include modapi/，绝不 include src/ 下任何引擎头文件。
// 能力取用：只用到方块能力（api->block.*）。
// ============================================================================

#include "modapi/mod_api.h"
#include "modapi/mod_c_api.h"

#include <cstdint>
#include <new>

namespace mod {

// 注册一个方块；失败返回 false（引擎侧已打详细原因，这里只记数量）。
static bool registerOne(const ModApi* api, const ModBlockDef& def) {
    const std::int32_t id = api->block.registerBlock(api->block.host, &def);
    return id >= 0;
}

class BuiltinBlocksMod {
public:
    const char* name() const { return "builtin_blocks"; }
    const char* version() const { return "1.0"; }

    bool onLoad(const ModApi* api) {
        if (api == nullptr) return false;
        if (api->block.version != kEngineApiVersion) {
            api->block.log(kLogError, "接口版本不匹配，拒绝加载");
            return false;
        }

        std::int32_t failed = 0;

        // ---- stone：石头（基准方块）----
        {
            ModBlockDef def{};
            def.name       = "stone";
            def.opaque     = 1U;
            def.hardness   = 1.5f;
            def.renderType = kRenderSolid;
            def.texture    = "stone";
            def.model      = nullptr;
            def.mass       = 2.5f;
            def.solid      = 1U;
            def.friction   = 0.7f;
            if (!registerOne(api, def)) ++failed;
        }

        // ---- wood：木头（模型化：顶/底 log_top，侧 log_side）----
        {
            ModBlockDef def{};
            def.name       = "wood";
            def.opaque     = 1U;
            def.hardness   = 2.0f;
            def.renderType = kRenderSolid;
            def.texture    = "log_side";
            def.model      = "wood";
            def.mass       = 1.2f;
            def.solid      = 1U;
            def.friction   = 0.5f;
            if (!registerOne(api, def)) ++failed;
        }

        // ---- dirt：泥土 ----
        {
            ModBlockDef def{};
            def.name       = "dirt";
            def.opaque     = 1U;
            def.hardness   = 0.5f;
            def.renderType = kRenderSolid;
            def.texture    = "dirt";
            def.model      = nullptr;
            def.mass       = 1.5f;
            def.solid      = 1U;
            def.friction   = 0.8f;
            if (!registerOne(api, def)) ++failed;
        }

        // ---- brick：砖块（注册表扩展性验收留下的方块）----
        {
            ModBlockDef def{};
            def.name       = "brick";
            def.opaque     = 1U;
            def.hardness   = 2.0f;
            def.renderType = kRenderSolid;
            def.texture    = "brick";
            def.model      = nullptr;
            def.mass       = 2.2f;
            def.solid      = 1U;
            def.friction   = 0.7f;
            if (!registerOne(api, def)) ++failed;
        }

        // ---- glass：玻璃（镂空渲染桶 + 不参与面剔除）----
        {
            ModBlockDef def{};
            def.name       = "glass";
            def.opaque     = 0U;
            def.hardness   = 0.5f;
            def.renderType = kRenderCutout;
            def.texture    = "glass";
            def.model      = nullptr;
            def.mass       = 1.0f;
            def.solid      = 1U;
            def.friction   = 0.3f;
            if (!registerOne(api, def)) ++failed;
        }

        // ---- grass：草地（地表层；顶 grass_top，侧 grass_side，底 dirt）----
        {
            ModBlockDef def{};
            def.name       = "grass";
            def.opaque     = 1U;
            def.hardness   = 0.6f;
            def.renderType = kRenderSolid;
            def.texture    = "grass_top";
            def.model      = "grass";
            def.mass       = 1.4f;
            def.solid      = 1U;
            def.friction   = 0.8f;
            if (!registerOne(api, def)) ++failed;
        }

        if (failed != 0) {
            // 不回滚：注册表没有 erase，已成功的方块保留，缺的由引擎侧 resolve 自检报出来。
            api->block.log(kLogError, "builtin_blocks 有方块注册失败（详见引擎日志）");
            return false;
        }

        api->block.log(kLogInfo, "builtin_blocks 已注册 6 个内置方块");
        return true;
    }

    void onUnload() {
        // 方块定义归引擎注册表管，本模组不持有堆资源。
    }
};

} // namespace mod

// ---- DLL 导出入口（纯 C 契约：ModVtbl + CreateModC，见 modapi/mod_c_api.h）----
// 五个静态转发函数 = 把 C 函数表接到本类的成员函数上。
// 它们在本 DLL 内编译，等价于 C++ 的静态分发，不跨模块。

static const char* builtinBlocksName(ModSelf self) {
    return static_cast<mod::BuiltinBlocksMod*>(self)->name();
}

static const char* builtinBlocksVersion(ModSelf self) {
    return static_cast<mod::BuiltinBlocksMod*>(self)->version();
}

static int32_t builtinBlocksOnLoad(ModSelf self, const void* api) {
    return static_cast<mod::BuiltinBlocksMod*>(self)->onLoad(
               static_cast<const ModApi*>(api))
               ? 0 : -1;
}

static void builtinBlocksOnUnload(ModSelf self) {
    static_cast<mod::BuiltinBlocksMod*>(self)->onUnload();
}

static void builtinBlocksDestroy(ModSelf self) {
    delete static_cast<mod::BuiltinBlocksMod*>(self);   // 谁 new 谁 delete：释放走本 DLL 的 CRT
}

static const ModVtbl kBuiltinBlocksVtbl{
    MODAPI_MOD_ABI_VERSION,
    &builtinBlocksName,
    &builtinBlocksVersion,
    &builtinBlocksOnLoad,
    &builtinBlocksOnUnload,
    &builtinBlocksDestroy,
};

extern "C" __declspec(dllexport) int32_t CreateModC(const void* api, ModSelf* outSelf,
                                                    const ModVtbl** outVtbl) {
    auto* instance = new (std::nothrow) mod::BuiltinBlocksMod();
    if (instance == nullptr) return -1;
    *outSelf = static_cast<ModSelf>(instance);
    *outVtbl = &kBuiltinBlocksVtbl;
    (void)api;   // Create 阶段不需要引擎能力：注册一律放 onLoad
    return 0;
}

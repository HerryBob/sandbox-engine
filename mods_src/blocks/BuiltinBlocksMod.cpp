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

#include "modapi/IMod.hpp"

#include <cstdint>
#include <new>

namespace mod {

// 注册一个方块；失败返回 false（引擎侧已打详细原因，这里只记数量）。
static bool registerOne(const modapi::ModApi* api, const modapi::ModBlockDef& def) {
    const std::int32_t id = api->block.registerBlock(api->block.host, &def);
    return id >= 0;
}

class BuiltinBlocksMod : public modapi::IMod {
public:
    const char* name() const override { return "builtin_blocks"; }
    const char* version() const override { return "1.0"; }

    bool onLoad(const modapi::ModApi* api) override {
        if (api == nullptr) return false;
        if (api->block.version != modapi::kEngineApiVersion) {
            api->block.log(modapi::kLogError, "接口版本不匹配，拒绝加载");
            return false;
        }

        std::int32_t failed = 0;

        // ---- stone：石头（基准方块）----
        {
            modapi::ModBlockDef def{};
            def.name       = "stone";
            def.opaque     = 1U;
            def.hardness   = 1.5f;
            def.renderType = modapi::kRenderSolid;
            def.texture    = "stone";
            def.model      = nullptr;
            def.mass       = 2.5f;
            def.solid      = 1U;
            def.friction   = 0.7f;
            if (!registerOne(api, def)) ++failed;
        }

        // ---- wood：木头（模型化：顶/底 log_top，侧 log_side）----
        {
            modapi::ModBlockDef def{};
            def.name       = "wood";
            def.opaque     = 1U;
            def.hardness   = 2.0f;
            def.renderType = modapi::kRenderSolid;
            def.texture    = "log_side";
            def.model      = "wood";
            def.mass       = 1.2f;
            def.solid      = 1U;
            def.friction   = 0.5f;
            if (!registerOne(api, def)) ++failed;
        }

        // ---- dirt：泥土 ----
        {
            modapi::ModBlockDef def{};
            def.name       = "dirt";
            def.opaque     = 1U;
            def.hardness   = 0.5f;
            def.renderType = modapi::kRenderSolid;
            def.texture    = "dirt";
            def.model      = nullptr;
            def.mass       = 1.5f;
            def.solid      = 1U;
            def.friction   = 0.8f;
            if (!registerOne(api, def)) ++failed;
        }

        // ---- brick：砖块（注册表扩展性验收留下的方块）----
        {
            modapi::ModBlockDef def{};
            def.name       = "brick";
            def.opaque     = 1U;
            def.hardness   = 2.0f;
            def.renderType = modapi::kRenderSolid;
            def.texture    = "brick";
            def.model      = nullptr;
            def.mass       = 2.2f;
            def.solid      = 1U;
            def.friction   = 0.7f;
            if (!registerOne(api, def)) ++failed;
        }

        // ---- glass：玻璃（镂空渲染桶 + 不参与面剔除）----
        {
            modapi::ModBlockDef def{};
            def.name       = "glass";
            def.opaque     = 0U;
            def.hardness   = 0.5f;
            def.renderType = modapi::kRenderCutout;
            def.texture    = "glass";
            def.model      = nullptr;
            def.mass       = 1.0f;
            def.solid      = 1U;
            def.friction   = 0.3f;
            if (!registerOne(api, def)) ++failed;
        }

        // ---- grass：草地（地表层；顶 grass_top，侧 grass_side，底 dirt）----
        {
            modapi::ModBlockDef def{};
            def.name       = "grass";
            def.opaque     = 1U;
            def.hardness   = 0.6f;
            def.renderType = modapi::kRenderSolid;
            def.texture    = "grass_top";
            def.model      = "grass";
            def.mass       = 1.4f;
            def.solid      = 1U;
            def.friction   = 0.8f;
            if (!registerOne(api, def)) ++failed;
        }

        if (failed != 0) {
            // 不回滚：注册表没有 erase，已成功的方块保留，缺的由引擎侧 resolve 自检报出来。
            api->block.log(modapi::kLogError, "builtin_blocks 有方块注册失败（详见引擎日志）");
            return false;
        }

        api->block.log(modapi::kLogInfo, "builtin_blocks 已注册 6 个内置方块");
        return true;
    }

    void onUnload() override {
        // 方块定义归引擎注册表管，本模组不持有堆资源。
    }
};

} // namespace mod

// ---- DLL 导出入口（C 链接，名字不修饰）----

extern "C" __declspec(dllexport) modapi::IMod* CreateMod(const modapi::ModApi* api) {
    return new (std::nothrow) mod::BuiltinBlocksMod();
}

extern "C" __declspec(dllexport) void DestroyMod(modapi::IMod* mod) {
    delete mod;
}

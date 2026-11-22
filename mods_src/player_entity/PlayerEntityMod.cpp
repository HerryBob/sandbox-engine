// ============================================================================
// 玩家实体模组：player_entity
// ============================================================================
// 角色外观与数值原先写死在引擎装配层（main.cpp 里一份路径 + InputSampler 里眼高/步速 +
// PhysicsWorld 里跳跃初速）。这一版把它们收进实体类型定义：
// 模型、贴图、碰撞箱、眼高、步速、跳速全部由本模组提供，删掉 mods/player_entity/ 就没有玩家外观。
//
// 只登记类型，不做任何渲染/物理动作：模组 onLoad 时 Vulkan 尚未初始化，
// 引擎在 vkCtx.init 之后才按类型表统一加载模型与贴图。
//
// 依赖纪律：只 include modapi/，绝不 include src/ 下任何引擎头文件。
// ============================================================================

#include "modapi/mod_api.h"
#include "modapi/mod_c_api.h"

#include <cstdint>
#include <new>

namespace mod {

class PlayerEntityMod {
public:
    const char* name() const { return "player_entity"; }
    const char* version() const { return "1.0"; }

    bool onLoad(const ModApi* api) {
        if (api == nullptr) return false;
        if (api->entity.version != kEngineApiVersion) {
            api->entity.log(kLogError, "接口版本不匹配，拒绝加载");
            return false;
        }

        // 路径相对本模组的 assets/ 根解析（引擎会拼成 mods/player_entity/assets/...）
        ModEntityDef def{};
        def.name       = "player";
        def.model      = "assets/models/entity/PlayerModel.bbmodel";
        def.texture    = "assets/textures/entity/Player.png";

        // 碰撞箱与现有物理一致（原 main.cpp / PhysicsWorld.cpp 的硬编码值）
        def.width      = 0.6f;
        def.height     = 1.8f;
        def.depth      = 0.6f;
        def.eyeHeight  = 1.62f;
        def.moveSpeed  = 4.3f;
        def.jumpSpeed  = 4.9f;

        // 动画回调留空：程序化动画阶段再接（引擎当前完全不调用）
        def.animUser   = nullptr;
        def.onAnimate  = nullptr;

        const std::int32_t id = api->entity.registerEntity(api->entity.host, &def);
        if (id < 0) {
            api->entity.log(kLogError, "player 实体类型注册失败（详见引擎日志）");
            return false;
        }

        api->entity.log(kLogInfo, "player_entity 已注册 player 实体类型");
        return true;
    }

    void onUnload() {
        // 类型定义归引擎注册表管，本模组不持有堆资源。
    }
};

} // namespace mod

// ---- DLL 导出入口（纯 C 契约：ModVtbl + CreateModC，见 modapi/mod_c_api.h）----

static const char* playerEntityModName(ModSelf self) {
    return static_cast<mod::PlayerEntityMod*>(self)->name();
}

static const char* playerEntityModVersion(ModSelf self) {
    return static_cast<mod::PlayerEntityMod*>(self)->version();
}

static int32_t playerEntityModOnLoad(ModSelf self, const void* api) {
    return static_cast<mod::PlayerEntityMod*>(self)->onLoad(
               static_cast<const ModApi*>(api))
               ? 0 : -1;
}

static void playerEntityModOnUnload(ModSelf self) {
    static_cast<mod::PlayerEntityMod*>(self)->onUnload();
}

static void playerEntityModDestroy(ModSelf self) {
    delete static_cast<mod::PlayerEntityMod*>(self);   // 谁 new 谁 delete：释放走本 DLL 的 CRT
}

static const ModVtbl kPlayerEntityModVtbl{
    MODAPI_MOD_ABI_VERSION,
    &playerEntityModName,
    &playerEntityModVersion,
    &playerEntityModOnLoad,
    &playerEntityModOnUnload,
    &playerEntityModDestroy,
};

extern "C" __declspec(dllexport) int32_t CreateModC(const void* api, ModSelf* outSelf,
                                                    const ModVtbl** outVtbl) {
    auto* instance = new (std::nothrow) mod::PlayerEntityMod();
    if (instance == nullptr) return -1;
    *outSelf = static_cast<ModSelf>(instance);
    *outVtbl = &kPlayerEntityModVtbl;
    (void)api;   // Create 阶段不需要引擎能力：注册一律放 onLoad
    return 0;
}

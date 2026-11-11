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

#include "modapi/IMod.hpp"

#include <cstdint>
#include <new>

namespace mod {

class PlayerEntityMod : public modapi::IMod {
public:
    const char* name() const override { return "player_entity"; }
    const char* version() const override { return "1.0"; }

    bool onLoad(const modapi::ModApi* api) override {
        if (api == nullptr) return false;
        if (api->entity.version != modapi::kEngineApiVersion) {
            api->entity.log(modapi::kLogError, "接口版本不匹配，拒绝加载");
            return false;
        }

        // 路径相对本模组的 assets/ 根解析（引擎会拼成 mods/player_entity/assets/...）
        modapi::ModEntityDef def{};
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
            api->entity.log(modapi::kLogError, "player 实体类型注册失败（详见引擎日志）");
            return false;
        }

        api->entity.log(modapi::kLogInfo, "player_entity 已注册 player 实体类型");
        return true;
    }

    void onUnload() override {
        // 类型定义归引擎注册表管，本模组不持有堆资源。
    }
};

} // namespace mod

// ---- DLL 导出入口（C 链接，名字不修饰）----

extern "C" __declspec(dllexport) modapi::IMod* CreateMod(const modapi::ModApi* api) {
    return new (std::nothrow) mod::PlayerEntityMod();
}

extern "C" __declspec(dllexport) void DestroyMod(modapi::IMod* mod) {
    delete mod;
}

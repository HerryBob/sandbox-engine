// ============================================================================
// 模组：mod_noise —— 噪声算法实现（perlin / fbm）
// ============================================================================
// 目的：把"噪声怎么算"从引擎里搬出来，交给 DLL。引擎只保留三件事：
//   ① 注册表（type 名 → create/sample/destroy 函数指针三元组）
//   ② 内置 flat（纯数学、恒返回 0，保证删掉所有 DLL 仍能跑平地世界）
//   ③ 列执行器（ColumnGenerator：问高度 → 按地层填方块）
//
// 两个 provider 全部经 NoiseApi::registerNoise 登记，与引擎内置 flat 地位平等：
//   perlin  —— 单层梯度噪声（质量好，略贵）
//   fbm     —— 多层叠加（基底固定为 perlin）
//
// 编译产物：build/Debug/mods/mod_noise.dll（由 CMake 的 mod_noise target 负责）
// 依赖纪律：只 include modapi/ 的头 + 本目录噪声实现头，绝不 include src/。
// 能力取用：本模组只需要噪声能力 —— 用 api->noise.*（日志/版本是基类字段）。
// 自包含纪律：fbm 组合基底不查引擎注册表 —— 否则会把引擎的函数指针拿进 DLL，
//             破坏"引擎侧删掉全部噪声实现也能编译"这一前提。
// ============================================================================

#include "modapi/IMod.hpp"
#include "modapi/noise_api.h"

#include "FbmNoise.hpp"
#include "PerlinNoise.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>

namespace mod {

// 失败原因回传（契约允许 errOut 为空指针）。
void setError(const char** errOut, const char* msg) {
    if (errOut != nullptr) *errOut = msg;
}

// 从参数数组里取数值参数（找不到返回 fallback）。count<=0 时 params 可为 nullptr。
double findNumber(const modapi::NoiseParamC* params, std::int32_t count,
                  const char* key, double fallback) {
    for (std::int32_t i = 0; i < count; ++i) {
        if (params[i].key != nullptr && std::strcmp(params[i].key, key) == 0) {
            return params[i].num;
        }
    }
    return fallback;
}

// 取字符串参数（找不到或该参数是数值时返回 nullptr）。
const char* findString(const modapi::NoiseParamC* params, std::int32_t count,
                       const char* key) {
    for (std::int32_t i = 0; i < count; ++i) {
        if (params[i].key != nullptr && std::strcmp(params[i].key, key) == 0) {
            return params[i].str;
        }
    }
    return nullptr;
}

// ---- perlin -------------------------------------------------------------
void* createPerlin(const modapi::NoiseParamC* params, std::int32_t count,
                   const char** errOut) {
    const double scale = findNumber(params, count, "scale", 1.0);
    // !(x > 0) 同时挡住 NaN —— 否则 NaN 会一路渗进高度计算
    if (!(scale > 0.0)) {
        setError(errOut, "perlin 的 scale 必须 > 0");
        return nullptr;
    }
    return new (std::nothrow) PerlinNoise(scale);
}

// ---- fbm ----------------------------------------------------------------
void* createFbm(const modapi::NoiseParamC* params, std::int32_t count,
                const char** errOut) {
    // 缺省值与原内置实现一致：基底 perlin、4 层、lacunarity 2、gain 0.5。
    const char* baseName = findString(params, count, "base");
    const int    octaves    = static_cast<int>(findNumber(params, count, "octaves", 4.0));
    const double scale      = findNumber(params, count, "scale", 1.0);
    const double lacunarity = findNumber(params, count, "lacunarity", 2.0);
    const double gain       = findNumber(params, count, "gain", 0.5);

    // 参数校验（原实现全无校验：octaves<=0 静默返回 0，gain>=1 归一化分母指数爆炸）
    if (octaves < 1 || octaves > 16) {
        setError(errOut, "fbm 的 octaves 必须在 1..16");
        return nullptr;
    }
    if (!(scale > 0.0)) {
        setError(errOut, "fbm 的 scale 必须 > 0");
        return nullptr;
    }
    if (!(lacunarity > 1.0)) {
        setError(errOut, "fbm 的 lacunarity 必须 > 1");
        return nullptr;
    }
    if (!(gain > 0.0 && gain < 1.0)) {
        setError(errOut, "fbm 的 gain 必须在 (0,1) 开区间内");
        return nullptr;
    }

    // 基底必须自包含：只认本 DLL 内的 perlin，绝不回调引擎注册表。
    const std::string base = (baseName != nullptr) ? baseName : "perlin";
    std::unique_ptr<NoiseSource> baseNoise;
    if (base == "perlin") {
        baseNoise = std::make_unique<PerlinNoise>(1.0);   // 频率交给 fbm 的 scale 统一控制
    } else {
        setError(errOut, "fbm 的 base 只支持 perlin");
        return nullptr;
    }

    return new (std::nothrow) FbmNoise(std::move(baseNoise), octaves, scale,
                                       lacunarity, gain);
}

// ---- sample / destroy：模板化，避免两份重复（模板只在 DLL 内部展开，不跨边界）----
template <typename T>
double sampleAny(void* self, double x, double z, std::uint64_t seed) {
    return static_cast<T*>(self)->sample(x, z, seed);
}

template <typename T>
void destroyAny(void* self) {
    delete static_cast<T*>(self);
}

// ---- provider 描述：必须是静态存储（引擎注册时会读 type 字符串指针）----
const modapi::NoiseProviderDef kPerlin{
    "perlin", &createPerlin, &sampleAny<PerlinNoise>, &destroyAny<PerlinNoise>,
};
const modapi::NoiseProviderDef kFbm{
    "fbm", &createFbm, &sampleAny<FbmNoise>, &destroyAny<FbmNoise>,
};

// ============================================================================
// 模组本体
// ============================================================================
class NoiseMod : public modapi::IMod {
public:
    const char* name() const override { return "mod_noise"; }
    const char* version() const override { return "0.1"; }

    bool onLoad(const modapi::ModApi* api) override {
        if (api == nullptr) return false;

        // 版本校验：接口一旦改版就拒绝加载，而不是带着错位布局继续跑。
        if (api->noise.version != modapi::kEngineApiVersion) {
            api->noise.log(modapi::kLogError, "接口版本不匹配，拒绝加载");
            return false;
        }
        if (api->noise.registerNoise == nullptr) {
            api->noise.log(modapi::kLogError, "引擎未提供 registerNoise 回调（版本过旧）");
            return false;
        }

        const modapi::NoiseProviderDef* defs[] = {&kPerlin, &kFbm};
        for (const modapi::NoiseProviderDef* def : defs) {
            if (api->noise.registerNoise(api->noise.host, def) != 0) {
                api->noise.log(modapi::kLogError,
                               (std::string("注册噪声 provider 失败：") + def->type).c_str());
                return false;
            }
        }

        api->noise.log(modapi::kLogInfo, "mod_noise 已注册 2 个噪声 provider（perlin/fbm）");
        return true;
    }

    void onUnload() override {
        // 三个 provider 的 def 是静态常量，引擎只拷贝了函数指针与 type 字符串。
        // 实例的销毁走各自的 destroy（引擎侧在句柄析构时调用），本模组无需清理。
    }
};

} // namespace mod

// ---- DLL 导出入口（C 链接，名字不修饰，host 用 GetProcAddress 精确查找）----

extern "C" __declspec(dllexport) modapi::IMod* CreateMod(const modapi::ModApi* api) {
    return new (std::nothrow) mod::NoiseMod();
}

extern "C" __declspec(dllexport) void DestroyMod(modapi::IMod* mod) {
    // 谁 new 谁 delete：这个 delete 必须发生在本 DLL 内，否则跨运行库释放会崩。
    delete mod;
}

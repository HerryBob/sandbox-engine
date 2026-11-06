#include "worldgen/noise/BuiltinNoiseTypes.hpp"

#include "core/Log.hpp"
#include "modapi/noise_api.h"
#include "worldgen/noise/NoiseRegistry.hpp"

#include <new>

namespace engine::worldgen {

namespace {

// ---------------------------------------------------------------------------
// flat：恒返回 0 的高度场（引擎唯一的"算法"）。
// 它不依赖任何噪声代码 —— baseHeight 直接就是地表高度；
// amplitude 乘 0 也仍然是 0，所以默认预设的 amplitude 对 flat 无意义。
// 用一个 1 字节哨兵表示"实例存在"：create/destroy 必须配对，契约要求非空句柄。
// ---------------------------------------------------------------------------

void* createFlat(const modapi::NoiseParamC*, std::int32_t, const char**) {
    return new (std::nothrow) char{0};
}

double sampleFlat(void*, double, double, std::uint64_t) {
    return 0.0;
}

void destroyFlat(void* self) {
    delete static_cast<char*>(self);
}

} // namespace

void initBuiltinNoiseTypes() {
    static const modapi::NoiseProviderDef kFlat{
        "flat", &createFlat, &sampleFlat, &destroyFlat,
    };
    if (!NoiseRegistry::registerType(kFlat)) {
        LOG_ERROR("Noise", "内置 flat 注册失败（重复注册或注册表已冻结）");
    }
}

bool selfTestNoiseTypes() {
    bool ok = true;

    // ① 内置 flat：可创建，且输出恒为 0（默认平地世界的地表来源）。
    {
        NoiseHandle handle;
        if (!NoiseRegistry::make("flat", nullptr, 0, handle)) {
            LOG_ERROR("Noise", "自检：flat 创建失败");
            ok = false;
        } else if (handle.sample(0.0, 0.0, 0) != 0.0) {
            LOG_ERROR("Noise", "自检：flat 的输出应为 0");
            ok = false;
        }
    }

    // ② 未注册类型：make 必须拒绝（引擎只查注册表，不硬编码任何类型名）。
    {
        NoiseHandle handle;
        if (NoiseRegistry::make("不存在的类型", nullptr, 0, handle)) {
            LOG_ERROR("Noise", "自检：未注册类型应被拒绝");
            ok = false;
        }
    }

    return ok;
}

} // namespace engine::worldgen

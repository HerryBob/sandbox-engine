#include "worldgen/noise/BuiltinNoiseTypes.hpp"

#include "worldgen/noise/FbmNoise.hpp"
#include "worldgen/noise/NoiseRegistry.hpp"
#include "worldgen/noise/PerlinNoise.hpp"
#include "worldgen/noise/ValueNoise.hpp"

#include <cstdio>
#include <memory>

namespace engine::worldgen {

void initBuiltinNoiseTypes() {
    NoiseRegistry::registerType("value", [](const NoiseParams& p) {
        return std::make_unique<ValueNoise>(p.get("scale", 1.0));
    });
    NoiseRegistry::registerType("perlin", [](const NoiseParams& p) {
        return std::make_unique<PerlinNoise>(p.get("scale", 1.0));
    });
    NoiseRegistry::registerType("fbm", [](const NoiseParams& p) -> std::unique_ptr<NoiseSource> {
        // 基底：递归查表实例化。当前只支持"嵌套名字"（基底参数走默认）；
        // 完整递归嵌套（base 携带参数）已登记为升级点，1a 保持现状。
        auto base = NoiseRegistry::make(p.base, NoiseParams{});
        if (!base) {
            std::fprintf(stderr, "[Noise] fbm 需要基底噪声，但 type=%s 未注册\n",
                         p.base.c_str());
            return std::unique_ptr<NoiseSource>(nullptr);
        }
        return std::make_unique<FbmNoise>(
            std::move(base),
            static_cast<int>(p.get("octaves", 4.0)),
            p.get("scale", 1.0),
            p.get("lacunarity", 2.0),
            p.get("gain", 0.5));
    });
}

bool selfTestNoiseTypes() {
    bool ok = true;

    // ① 确定性：同 seed + 同参数 + 同点 → 两次采样完全一致（流式命脉）。
    {
        NoiseParams p;
        p.values["scale"] = 0.01;
        auto a = NoiseRegistry::make("perlin", p);
        auto b = NoiseRegistry::make("perlin", p);
        if (!a || !b) {
            std::fprintf(stderr, "[Noise 自检] perlin 实例化失败\n");
            ok = false;
        } else if (a->sample(1.5, -2.25, 42) != b->sample(1.5, -2.25, 42)) {
            std::fprintf(stderr, "[Noise 自检] perlin 确定性破坏：同点两次采样不一致\n");
            ok = false;
        }
    }

    // ② 未注册类型：make 返回空（引擎查表，不硬编码任何类型）。
    {
        NoiseParams p;
        if (NoiseRegistry::make("不存在的类型", p)) {
            std::fprintf(stderr, "[Noise 自检] 未注册类型应返回 nullptr\n");
            ok = false;
        }
    }

    // ③ fbm 包 perlin：组合子链路（base 递归实例化）+ 输出范围 [-1,1]。
    {
        NoiseParams p;
        p.base = "perlin";
        p.values["octaves"] = 4.0;
        p.values["scale"] = 0.01;
        auto f = NoiseRegistry::make("fbm", p);
        if (!f) {
            std::fprintf(stderr, "[Noise 自检] fbm(perlin) 实例化失败\n");
            ok = false;
        } else {
            const double v = f->sample(3.0, 4.0, 7);
            if (v < -1.0 || v > 1.0) {
                std::fprintf(stderr, "[Noise 自检] fbm 输出越界：%f\n", v);
                ok = false;
            }
        }
    }

    return ok;
}

} // namespace engine::worldgen

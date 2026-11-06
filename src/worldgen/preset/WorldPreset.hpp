#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine::worldgen {

// 地层：一个方块在"相对地表深度"区间的填充。
// depthMin/depthMax 为相对地表深度（0 = 地表）——#36 决策点 1 已定案为"相对地表"，
// 不再是草案（绝对高度区间的备选方案已弃用；代码与 default.json 均按此语义跑通）。
struct Stratum {
    std::string block;    // 方块注册名（如 "grass" / "dirt" / "stone"）
    int depthMin = 0;     // 相对地表深度下界（含）
    int depthMax = 0;     // 相对地表深度上界（含）
};

// 高度参数：JSON 里 noise 节点的"其余参数"，数值或字符串二选一。
// 引擎不认识具体键 —— 参数怎么解释由 provider（DLL 或内置 flat）自己决定。
// 纯数据、可拷贝：预设对象会被 ColumnGenerator 按值持有（生成线程安全）。
struct HeightParam {
    std::string key;      // 参数名（如 "scale" / "octaves" / "base"）
    std::string str;      // 非空 = 字符串参数（如 fbm 的基底类型名）
    double      num = 0.0; // str 为空时使用
};

// 世界预设（纯数据装配结果）：一个世界怎么生成的全部参数。
// 来源 = data/worldgen/presets/<name>.json（PresetLoader 解析）。
//
// 关键设计：这里只存"该调谁、传什么参数"，不存任何已实例化的噪声对象 ——
// 实例化发生在 ColumnGenerator::prepare()，那时 DLL 必然已加载完毕。
// 因此本结构可拷贝、可跨线程持有（这正是生成线程只读访问的前提）。
struct WorldPreset {
    std::uint64_t seed = 0;

    // 高度场：provider 名 + 可选基底 + 其余参数。
    std::string heightType;                 // JSON noise.type（"flat" / "perlin" / "fbm" ...）
    std::string heightBase;                 // JSON noise.base（fbm 的基底；空 = 无嵌套）
    std::vector<HeightParam> heightParams;  // 其余参数（scale/octaves/lacunarity/gain ...）

    double baseHeight = 32.0;               // 高度映射：基准高度（起伏中点）
    double amplitude = 16.0;                // 高度映射：起伏幅度（±）
    std::vector<Stratum> strata;            // 地层表（按相对地表深度）
};

} // namespace engine::worldgen

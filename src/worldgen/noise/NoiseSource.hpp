#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace engine::worldgen {

// 噪声源抽象接口（noise source）：地形生成只认识这个契约，不认识任何具体噪声算法。
// 三层解耦：模组注册新 type ↔ 数据文件编排 → 引擎只调 sample()（见《世界生成数据驱动架构决策.md》）。
class NoiseSource {
public:
    virtual ~NoiseSource() {}   // 虚析构（多态删除安全：delete 基类指针时正确派发到派生析构）

    // 2D 高度图采样（1a 只需要高度；3D 密度场将来加接口，不提前——触发器模式）。
    // x/z：世界坐标（未缩放）；seed：确定性种子 —— 同 seed + 同参数 = 同世界（流式命脉）。
    virtual double sample(double x, double z, std::uint64_t seed) const = 0;
};

// 工厂参数：JSON 数值字段的通用承载（PresetLoader 解析时填充）。
// 设计取舍：不用定长 struct —— 引擎不认识具体 type，也就不认识具体参数键；
// 内置噪声从 values 里取自己认识的键（取不到回默认），模组新噪声同样自由取。
struct NoiseParams {
    std::string type;                                // 本节点 type（解析回读，日志/调试用）
    std::string base;                                // 嵌套基底 type（fbm 用；空 = 无嵌套）
    std::unordered_map<std::string, double> values;  // 其余数值参数（scale/octaves/...）

    // 取数值参数，缺省回默认值（键名拼错 = 静默回默认；关键键由 PresetLoader 校验）。
    double get(const char* key, double fallback) const {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
};

} // namespace engine::worldgen

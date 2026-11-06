#pragma once

#include <cstdint>

namespace mod {

// 噪声源抽象接口（noise source）：DLL 内部的实现基类。
// 注意：这个类不跨 DLL 边界 —— 引擎侧看到的是 modapi/noise_api.h 里的
// 函数指针三元组（create / sample / destroy），引擎不认识任何 C++ 类。
// 本类只用于 DLL 内部把 perlin / fbm 串起来（fbm 需要组合基底）。
class NoiseSource {
public:
    virtual ~NoiseSource() {}   // 虚析构（多态删除安全：delete 基类指针时正确派发到派生析构）

    // 2D 高度图采样。x/z：世界坐标（未缩放）；seed：确定性种子 ——
    // 同 seed + 同参数 = 同世界（流式命脉）。
    // const + 无共享可变状态 = 可被生成线程并发调用。
    virtual double sample(double x, double z, std::uint64_t seed) const = 0;
};

} // namespace mod

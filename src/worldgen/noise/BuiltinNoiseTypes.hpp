#pragma once

namespace engine::worldgen {

// 引擎内置噪声 provider：只剩 flat（纯数学，恒返回 0）。
// 为什么 flat 留在引擎：自举 —— 删掉 mods/ 下所有 DLL，引擎仍然能跑出平地世界。
// value / perlin / fbm 的实现已搬进 mod_noise.dll，与模组走同一注册入口、地位平等。
void initBuiltinNoiseTypes();

// 行为自检：flat 可创建且恒为 0；未注册类型一律被拒（引擎不硬编码任何类型名）。
// 失败写 stderr；全部通过返回 true（成功信息由装配层打印）。
bool selfTestNoiseTypes();

} // namespace engine::worldgen

#pragma once

namespace engine::worldgen {

// 内置噪声注册（引擎第一方内容）：value / perlin / fbm。
// 引擎装配阶段调用（freeze 之前）；与模组 DLL 注册同一入口、地位平等。
// 内置三件套永不移进 DLL（自举：无模组时引擎也有默认世界；也是模组的基底材料）。
void initBuiltinNoiseTypes();

// #33 行为自检：注册→查表→未注册报错→fbm 组合链路。
// 失败写 stderr；全部通过返回 true（成功信息由装配层打印）。
bool selfTestNoiseTypes();

} // namespace engine::worldgen

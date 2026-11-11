#pragma once

#include "modapi/api_base.h"

#include <cstdint>

namespace modapi {

// 单根骨骼的变换输出（动画回调用）。
struct ModBonePose {
    const char*   bone;      // 骨骼名（head / body / right_arm / left_arm / right_leg / left_leg）
    float         yawDeg;    // 绕 Y 轴旋转（度）
    float         pitchDeg;  // 绕 X 轴旋转（度）
    float         rollDeg;   // 绕 Z 轴旋转（度）
};

// 实体类型定义：描述"这类生物长什么样、怎么动"。
// 一个定义可实例化多个实例；玩家的唯一性属于实例层，不写在这里。
struct ModEntityDef {
    const char*   name;       // 实体类型名
    const char*   model;      // bbmodel 路径（相对资源根）
    const char*   texture;    // 覆盖贴图；nullptr = 用模型自己寻址的图
    float         width;      // 碰撞箱宽（米）
    float         height;     // 碰撞箱高（米）
    float         depth;      // 碰撞箱深（米）
    float         eyeHeight;  // 眼高（米，相机贴脸用）
    float         moveSpeed;  // 水平移动速度（格/秒）
    float         jumpSpeed;  // 跳跃初速度（格/秒）

    // ---- v5 追加：动画回调（引擎当前不调用，程序化动画阶段再接）----
    void*         animUser;   // 回调上下文，原样回传
    // 每帧动画求值：返回实际写入的骨骼数；nullptr 表示该实体不提供动画。
    // 参数：user=回调上下文 / dtSec=本帧时长 / speed=当前水平速度(格每秒) /
    //       out=骨骼变换数组（调用方分配）/ capacity=数组容量
    std::uint32_t (*onAnimate)(void* animUser, float dtSec, float speed,
                               ModBonePose* out, std::uint32_t capacity);
};

// 实体注册能力：继承共用基类。
struct EntityApi : ApiBase {
    // 注册一个实体类型：成功返回类型 id（>= 0）；失败 -1。
    std::int32_t (*registerEntity)(void* host, const ModEntityDef* def);
};

} // namespace modapi

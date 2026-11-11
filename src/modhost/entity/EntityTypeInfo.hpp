#pragma once

#include "modapi/entity_api.h"

#include <cstdint>
#include <string>

namespace engine::modhost {

// 实体类型定义（引擎侧副本）：来自模组注册。
// 字符串全部转 std::string 副本，绝不留 DLL 里的指针（卸载后悬空）。
// 一个类型可实例化多个实体；玩家唯一性属于实例层，不在本结构里。
struct EntityTypeInfo {
    std::string name;      // 实体类型名
    std::string model;     // bbmodel 路径（相对资源根）
    std::string texture;   // 覆盖贴图路径；空串 = 用模型自己寻址的图
    float       width = 0.6f;      // 碰撞箱宽（米）
    float       height = 1.8f;     // 碰撞箱高（米）
    float       depth = 0.6f;      // 碰撞箱深（米）
    float       eyeHeight = 1.62f; // 眼高（米）
    float       moveSpeed = 4.3f;  // 水平移动速度（格/秒）
    float       jumpSpeed = 4.9f;  // 跳跃初速度（格/秒）

    // 动画回调（v5 字段，引擎当前不调用；程序化动画阶段再接）
    void*       animUser = nullptr;
    std::uint32_t (*onAnimate)(void* animUser, float dtSec, float speed,
                               modapi::ModBonePose* out, std::uint32_t capacity) = nullptr;
};

} // namespace engine::modhost

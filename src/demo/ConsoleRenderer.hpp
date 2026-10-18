#pragma once
#include "shared/Block.hpp"

#include <vector>

namespace engine::demo {

// 伪渲染（第 4 轨只读演示）：x-y 侧视 ASCII 帧。
// 渲染线程落地前的占位：主线程按节奏读取世界快照，输出控制台侧视图，
// 验证"第 4 轨只读"的数据通路（锁内快照，不阻塞业务/物理轨）。
// 视口 x 跟随飞船（飞船居中）；z 层投影（重叠格只画一次）。
// 字符：S=飞船区域，#=其他方块，空格=空气。
void renderFrame(const std::vector<engine::shared::Block>& blocks,
                 double shipX, int state, int tick);

} // namespace engine::demo

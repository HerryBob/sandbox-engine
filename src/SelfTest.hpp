#pragma once
// 装配期自检：不依赖窗口 / GPU / 磁盘，纯内存断言。
// 全部在建 VulkanContext 之前跑；返回 false 表示某条机制被破坏，
// 由调用方决定是打印告警还是终止启动。

namespace engine::selftest {

// ChunkData：坐标编解码互逆 + 调色板 4→8→16 两档扩宽 + 写回空气后判空。
bool palette();

// 元素级旋转的数学：绕轴旋转落点正确 + 旋转原点自身不动。
bool modelRotation();

// 元素级旋转的几何落点：rotation 字段确实进到顶点（跑 buildModelVertices 量包围盒）。
bool modelGeometry();

// 刚体网格：3×3×3 内部接触面全剔除 → 54 面 / 216 顶点。
bool bodyMesh();

// 模组排序：批次倒序依赖拒绝、跨批放行、缺失依赖拒绝、同批环双方拒绝。
bool modSorter();

} // namespace engine::selftest

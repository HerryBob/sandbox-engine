#pragma once
// 装配期自检：不依赖窗口 / GPU / 磁盘，纯内存断言。
// 全部在建 VulkanContext 之前跑；返回 false 表示某条机制被破坏，
// 由调用方决定是打印告警还是终止启动。
//
// 来源：本文件原在 src/SelfTest.hpp（与 main.cpp 同级的根目录）。
// 规则1 第二轮（同类型文件归一）移入 core/ —— 自检是跨领域的基础设施，
// 与 Log / SPSCQueue / ThreadLoop / TimeManager 同类。CMake 用 GLOB_RECURSE，
// 移动无需改构建脚本。

namespace engine::selftest {

// ChunkData：坐标编解码互逆 + 调色板 4→8→16 两档扩宽 + 写回空气后判空。
bool palette();

// 元素级旋转的数学：绕轴旋转落点正确 + 旋转原点自身不动。
bool modelRotation();

// 元素级旋转的几何落点：rotation 字段确实进到顶点（跑 buildModelVertices 量包围盒）。
bool modelGeometry();

// 刚体网格：3×3×3 内部接触面全剔除 → 54 面 / 216 顶点。
bool bodyMesh();

  // 世界 chunk 网格（ChunkMeshBuilder，纯 CPU）：3×3×3 实心 → 54 面 / 216 顶点；
  // 单个孤立方块 → 6 面 / 24 顶点；空输入 → 空。
  // 这是渲染层拆分后第一个**可单测**的点（不需要窗口/GPU）。
  bool chunkMesh();

  // 存档格式：ChunkData 序列化 round-trip（全空 / 4 位 / 8 位 / 16 位）+ 损坏拒绝。
  bool chunkSerialize();

  // 存档目录：创建/写 chunk/读回比对/level.json 往返/缺失与损坏都拒绝。
  bool saveDir();

  // 存档菜单：新建/选择/两次确认删除/退出；非存档目录不被误判。
  bool saveMenu();

  // 图集 UV：各格 UV 矩形必须整块落在格内（防跨格采样取到隔壁纹理）。
  bool atlasUV();

  // 拾取裁决：两条射线结果 → 唯一动作（刚体/方块/近等距/超时/双空）。
  bool pickResolve();

// 模组排序：批次倒序依赖拒绝、跨批放行、缺失依赖拒绝、同批环双方拒绝。
bool modSorter();

} // namespace engine::selftest

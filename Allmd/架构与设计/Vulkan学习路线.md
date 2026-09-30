# Vulkan 学习路线（分层策略，2026-09-30 定）

> 原则：**初始化样板会读会改即可；心智模型必须深入；性能细节用到再学。**
> 与"重构不做 Big Bang"同源：只学当前里程碑需要的，遇到瓶颈再专项深挖。

## 一、三层划分

| 层 | 内容 | 学习程度 |
|---|---|---|
| **① 样板代码** | 实例/设备/交换链的结构体填充、参数枚举、错误码含义 | 会读会改，查文档照抄即可，**不用背** |
| **② 心智模型** | 对象生命周期/创建销毁顺序、队列族、同步（Fence/Semaphore）、每帧循环、renderpass/pipeline/命令录制交互 | **必须深入**——决定架构决策，错了出深层 bug 却找不到原因 |
| **③ 性能细节** | 内存管理（VMA）、descriptor 池、多帧 in-flight、pipeline cache、动态渲染、实例化绘制 | 帧率不够/瓶颈出现时再专项学习 |

## 二、深入时机表

| 阶段 | 深入什么 | 理解到什么程度 |
|---|---|---|
| 初始化链（窗口/实例/设备/交换链） | ①心智模型：生命周期、队列族 | 一次写透 + 封装成 `VulkanContext`，之后不动 |
| 第一个三角形 | 命令录制→提交→呈现（每帧循环） | **弄懂一次**，封装 drawFrame() |
| 方块 mesh | 顶点缓冲/索引/描述符 | 体素引擎技术含量所在——重点 |
| 几万方块 | 实例化绘制/视锥剔除 | 性能问题出现时再深入 |
| 帧率瓶颈 | 同步/内存/多帧流水线 | 届时专项学习 |

## 三、里程碑进度（2026-09-30）

```
✅ GLFW 窗口        （1280x720，GLFW_NO_API 纯 Vulkan 窗口）
✅ volk 动态加载     （免 LunarG SDK，加载 vulkan-1.dll）
✅ 实例 + 验证层     （surface 扩展预启用；验证层驱动不带则跳过）
✅ 物理设备枚举      （RX 6750 GRE 12GB，API 1.4.315）
✅ 逻辑设备 + 队列   （graphics+present 同族 #0，容量 8；swapchain 扩展启用）
✅ surface          （窗口↔GPU 连接点）
✅ 交换链 + 图像视图 （3 图像双缓冲+1，B8G8R8A8_SRGB，FIFO vsync）
✅ RenderPass + Pipeline（CLEAR→画→STORE 呈现规则；着色器 shaderc 运行时编译免 SDK）
✅ 第一个方块！      （24 顶点 36 索引，6 面独立着色；顶点/索引缓冲 HOST_VISIBLE；
                     每帧 acquire→录制→drawIndexed→submit→present 闭环，36/36 断言）
✅ 相机 + MVP uniform（descriptor set + UBO；glm 透视/视图矩阵；WASD 平移 +
                     鼠标左键拖动旋转 + R 重置；proj[1][1]*=-1 适配 Vulkan Y 向下 NDC；
                     37/37 断言）
⬜ chunk mesh：方块批量绘制 + 相邻面剔除
⬜ 纹理（stb_image 图集 + UV）
```

## 四、已沉淀的可复用封装

- `src/render/VulkanContext.{hpp,cpp}`：初始化链 + surface + 交换链 + 图像视图 + RenderPass + 图形管线 + 帧缓冲 + 顶点/索引缓冲 + 命令 + 同步 + drawFrame 每帧循环（销毁顺序：render→swapchain→device→surface→instance→window）
- 坑记录：
  - `VK_NO_PROTOTYPES` 必须在 vulkan.h 前定义（volk 硬性要求）
  - `VK_USE_PLATFORM_WIN32_KHR` 不定义则 win32 surface 头不引入
  - 旧版带 `_KHR` 后缀的枚举（如 `VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR`）兼容性最好
  - vcpkg shaderc 的 CMake target 是 `unofficial::shaderc::shaderc`（不是 shaderc_combined）
  - 相机默认朝向必须面向 -Z（`lookDir.z = -cos(yaw)*cos(pitch)`）：GLFW/Vulkan 惯例 +Z 是"屏幕里"方向，初始朝向 +Z 会看向方块背后 → 全黑窗口（已踩坑修复）
  - GLFW 输入回调必须是 static 成员/自由函数（C 函数指针），经 `glfwSetWindowUserPointer` 转发到对象；放匿名 namespace 里会编译报 C2888

## 五、与引擎架构的对应

- 队列族 ≈ 业务/物理/输入线程（GPU 的"工人班组"）
- 每帧循环 ≈ 渲染线程（第 4 轨 60Hz）每 tick 完整跑一次
- 脏 chunk → 重建 mesh → 顶点缓冲（体素 = 方块 6 面三角化）
- 刚体位置 → glm 模型矩阵 → 顶点着色器

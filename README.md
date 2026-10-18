# 沙盒引擎（Sandbox Engine）

C++ 自研方块沙盒引擎（对标 Minecraft）：**四线程消息流 + Vulkan 渲染 + 动态复合体**（任意方块打包为单个刚体，对标机械动力航空学）。

## 当前状态（2026-10-19）

- **四轨消息流跑通**：输入墙钟采样 / 物理 25TPS / 业务 25TPS / 渲染只读观众（SPSC 无锁队列，命令零丢失）
- **区块化存储**：16³ 稀疏 chunk + 脏标记 + 变更事件流（业务 → 渲染增量通道）
- **Vulkan 全链渲染**：交换链 / MSAA 8x / 深度缓冲 / 背面剔除 / 方块 mesh / 相机 MVP
- **进行中**：放置/破坏方块交互闭环（准星模式输入 → DDA 射线检测 → 业务裁决 → 变更事件 → mesh 重建）

## 文档导航

| 文档 | 内容 |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 当前架构决策（四轨 / 决策 D1-D15 / 踩坑记录 / 验证基线）——**权威文档** |
| [docs/vulkan-learning-roadmap.md](docs/vulkan-learning-roadmap.md) | Vulkan 学习路线（三层策略 + 里程碑进度 + 坑记录） |
| [docs/references/minetest-chunk-mesh-analysis.md](docs/references/minetest-chunk-mesh-analysis.md) | Minetest 区块存储与客户端 Mesh/LOD 参考分析 |
| [docs/legacy/沙盒引擎架构设计-历史蓝图.md](docs/legacy/沙盒引擎架构设计-历史蓝图.md) | 早期三轨蓝图（已过时，保留远期设计参考） |

## 构建

1. **VS2026** → 文件 → 打开 → 文件夹 → 选中本目录（含 `CMakeLists.txt` 这层）
2. 顶部配置下拉选 `x64-Debug`（或 Release），生成并运行 `minecraft_engine`

命令行：

```
cmake --build E:\MinecraftGame2\build --config Debug
# exe: E:\MinecraftGame2\build\Debug\minecraft_engine.exe
# 注意：运行工作目录需切到 exe 所在目录，以解析 shaders/ textures/ 相对路径
```

依赖：vcpkg manifest（glm / glfw3 / vulkan-headers + volk / shaderc），Vulkan 免 SDK 动态加载。

## 仓库状态

私有仓库（GitHub：HerryBob/sandbox-engine），原型阶段，个人开发。公开只读前暂不接受 PR。

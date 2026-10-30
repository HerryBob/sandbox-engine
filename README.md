# 沙盒引擎（Sandbox Engine）

C++ 自研方块沙盒引擎（对标 Minecraft）：**四线程消息流 + Vulkan 渲染 + 数据驱动模型 + 纹理图集 + 动态复合体**（任意方块打包为单个刚体，对标机械动力航空学）。

## 当前状态（2026-10-31）

- **四轨消息流跑通**：输入墙钟采样 / 物理 25TPS / 业务 25TPS / 渲染只读观众（SPSC 无锁队列，命令零丢失）
- **区块化存储**：16³ 稀疏 chunk + 脏标记 + 变更事件流 + **增量重建**（业务标脏 → GPU 安全点重建脏 chunk，全量重建是增量的特例）
- **Vulkan 全链渲染**：交换链 / MSAA 8x / 深度缓冲 / 背面剔除 / **Solid + Cutout 双管线分桶**（玻璃等镂空方块走 Cutout，双面渲染）
- **纹理图集**：16×16 / 8×8 格 Atlas（升级分辨率只改一个常量），名字驱动单向流（BlockInfo.texture → 模型点名 → 图集备货 → meshing 碰头）
- **数据驱动方块模型**：Model 三件套（Def / Loader / Registry）+ `assets/models/block/*.json`（MC 式资源编排，文件名即注册键）+ buildModelVertices 分流（模型驱动 vs 立方体兜底）
- **放置/破坏方块交互闭环**：准星模式输入 → DDA 射线检测 → 业务裁决（base/BlockActions）→ 变更事件 → 增量 mesh 重建
- **设计定案（未实现，等有消费者）**：流式加载与驻留架构——驻留/活跃/渲染三轴独立 + 租约制（玩家窗口只是默认租约之一）+ WorldManager 复合体，详见下方文档

## 文档导航

| 文档 | 内容 |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 当前架构决策（四轨 / 决策 D1-D19 / 踩坑记录 / 验证基线 / 路线图）——**权威文档** |
| [docs/DOCS自制版/流式加载与驻留架构决策.md](docs/DOCS自制版/流式加载与驻留架构决策.md) | 流式加载与自由驻留架构（三轴独立 / 租约三原语 / WorldManager 复合体 / 市面对照） |
| [docs/DOCS自制版/模型格式决策.md](docs/DOCS自制版/模型格式决策.md) | 数据驱动方块模型格式（ModelDef / JSON 结构 / 纹理变量表 `#展开`） |
| [docs/DOCS自制版/玩法决策.md](docs/DOCS自制版/玩法决策.md) | 玩法方向与引擎通用原语（能源三态 / 结构物理 / 流体气体） |
| [docs/DOCS自制版/流体与气体系统设计.md](docs/DOCS自制版/流体与气体系统设计.md) | 流体/气体系统设计（chunk 是流体、存档、LOD 的共同地基） |
| [docs/vulkan-learning-roadmap.md](docs/vulkan-learning-roadmap.md) | Vulkan 学习路线（三层策略 + 里程碑进度 + 坑记录） |
| [docs/references/minetest-chunk-mesh-analysis.md](docs/references/minetest-chunk-mesh-analysis.md) | Minetest 区块存储与客户端 Mesh/LOD 参考分析 |
| [docs/legacy/沙盒引擎架构设计-历史蓝图.md](docs/legacy/沙盒引擎架构设计-历史蓝图.md) | 早期三轨蓝图（已过时，保留远期设计参考） |

## 构建

1. **VS2026** → 文件 → 打开 → 文件夹 → 选中本目录（含 `CMakeLists.txt` 这层）
2. 顶部配置下拉选 `x64-Debug`（或 Release），生成并运行 `sandbox_engine`

命令行：

```
cmake --build E:\MinecraftGame2\build --config Debug
# exe: E:\MinecraftGame2\build\Debug\sandbox_engine.exe
# 注意：运行工作目录需切到 exe 所在目录；assets/ 已由 CMake copy_directory 拷贝到 exe 旁（models/ textures/ 资源相对路径从 exe 解析）
```

依赖（不入库）：LunarG Vulkan SDK（通过 `VULKAN_SDK` 环境变量定位）+ 仓库内置 third_party（GLM / GLFW / RapidJSON / stb_image，许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)）。

> 公开版仓库**不含纹理资源**（原 MC 版权纹理已移除；本地构建自备贴图）。缺纹理时编译不受影响，运行时缺失贴图走透明黑/棋盘格兜底（图集加载自动降级）。

## AI 协助开发说明

本项目为**作者主导设计、AI 辅助开发**的学习型项目：

- 架构决策（四轨消息流、业务权威裁决、区块化存储、打包/解包复合体、流式加载与驻留架构等）由作者确定，AI 提供方案比对与权衡分析
- AI 承担：代码生成辅助、逐行教学讲解、bug 排查建议、文档整理
- **所有代码均经作者通读、理解、审查后提交**（非原样照搬）；多线程与关键路径逻辑经作者人工 review
- 公开目的：学习记录 + 能力展示

## 免责声明

本项目是个人学习项目，**与 Mojang Studios / Minecraft 无任何关联，非官方项目**；"Minecraft" 仅作技术对标引用。本项目不使用、不包含任何 Minecraft 游戏资源（纹理/音效/模型）。
作者: 只是作为学习项目, 价值并不大, 如果有人想要fork并再开发, 十分欢迎, 但也需告知我一下。

## 仓库状态

公开只读仓库（GitHub：HerryBob/sandbox-engine），原型阶段，个人开发。暂不接受 PR，欢迎 Bug Issue。

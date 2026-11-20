# 沙盒引擎（Sandbox Engine）

C++ 自研方块沙盒引擎（对标 Minecraft）：**四线程消息流 + Vulkan 渲染 + 数据驱动模型 + 纹理图集 + 动态复合体**（任意方块打包为单个刚体，对标机械动力航空学）。

## 当前状态（2026-11-20）

### 已跑通

- **四轨消息流**：输入墙钟采样 / 物理 25TPS / 业务 25TPS / 渲染只读观众（SPSC 无锁队列，命令零丢失）
- **区块化存储**：16³ 调色板压缩 chunk + 脏标记 + 变更事件流 + **增量重建**（取数 = dirty ∪ 6 邻居，单次重建约 2.9 万槽位，见 D22）
- **Vulkan 全链渲染**：交换链 / MSAA 8x / 深度缓冲 / **Solid + Cutout 双管线分桶**（玻璃等镂空方块走 Cutout，双面渲染）
- **纹理图集**：16×16 格 Atlas（升级分辨率只改一个常量），名字驱动单向流（BlockInfo.texture → 模型点名 → 图集备货 → meshing 碰头）
- **数据驱动方块模型**：Model 三件套 + `assets/models/block/*.json` + parent 烘焙 + 声明式图集 + buildModelVertices 分流（模型驱动 vs 立方体兜底）
- **世界生成（数据驱动 + 流式）**：预设 JSON → 噪声注册表（provider 在 DLL 里）→ 后台生成线程纯计算（**不持有 World**）→ SPSC 回传 → 业务线程唯一写入；实测预热 81 列 / 220 chunk / 69.5 万方块，移动 1 列后 247 chunk / 78 万方块，物理 TPS 24.83–24.99
- **放置 / 破坏闭环**：准星模式 → DDA 射线 → 业务裁决（base/BlockActions）→ 变更事件 → 增量 mesh 重建
- **玩家实体化**：bbmodel + 逐骨骼渲染 + 骨骼动画回调点；实体类型表定义体型与手感（碰撞箱 0.6×1.8×0.6 / 眼高 1.62 / 步速 4.3 / 跳速 4.9），由内容注入而非引擎写死；第一 / 第三人称相机
- **模组系统**：单一 DLL 通道 + `modapi` C ABI v5（block / noise / world / entity 四类能力）+ 清单解析与依赖排序 + **按来源回滚**；噪声（perlin/fbm）与玩家实体均已外置为模组 —— **删掉全部 DLL 仍能跑平地世界**（内置 `flat` 是自举基线）
- **启动自检**：刚体网格 / 调色板 / 模型旋转 / 模组排序 / 噪声注册全部收拢到 `SelfTest`

### 进行中（**有意保留的过渡态**）

- **物理求解器已下线**：手写积分、单 AABB 碰撞与回退、接触集合差集、运动学推进、`moveKinematicAxis`、`recomputeAABB` 全部删除（`PhysicsWorld.cpp` 561 → 366 行），只保留**时间推进 / 指令翻译 / 回执组装 / 玩家 seqlock**。
  **玩家与刚体当前不会移动** —— 等待 Jolt 5.6 接入（迁移规格已定，见下方文档）
- **渲染侧已就绪**：刚体位姿支持四元数旋转（协议 → 桥 → 渲染三层贯通），相机矩阵 CPU 预乘为 `viewProj`（UBO 192 → 64 字节，顶点着色器只剩一次矩阵乘）

### 设计定案（未实现）

Jolt 物理迁移、玩法减法清单、模组化分层与 API 两层、模组 API 演进策略（双版本号 + C 入口）、风系统、流体 / 气体、流式加载与驻留（租约制 + WorldManager 复合体）


## 文档导航

| 文档 | 内容 |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 当前架构决策（四轨 / 决策 D1-D22 / 踩坑记录 / 验证基线）——**权威文档** |
| [docs/DOCS自制版/README.md](docs/DOCS自制版/README.md) | **全部设计文档的索引**（状态图例：已落地 / 部分落地 / 设计未实现），从这里进 |
| [未来方向-通读之后的路线.md](docs/DOCS自制版/未来方向-通读之后的路线.md) | **总路线**：state 化 → Jolt → 存档 → 原型 → 优化 |
| [未来方向-物理内核替换为Jolt.md](docs/DOCS自制版/未来方向-物理内核替换为Jolt.md) | Jolt 5.6 迁移：分层边界、v1 范围、睡眠机制与运动类型记录、物理 LOD |
| [架构决策-模组化分层.md](docs/DOCS自制版/架构决策-模组化分层.md) | 模组化总纲（机制 / 内容判据、引擎保留项、API 两层） |
| [架构决策-模组API演进.md](docs/DOCS自制版/架构决策-模组API演进.md) | API 演进：双版本号（ABI / 能力）+ C 入口 + 扩张→冻结→重构节奏 |
| [模组架构决策.md](docs/DOCS自制版/模组架构决策.md) | 模组加载器定位（单一 DLL 通道）+ 开放面做厚 + 内置 vs 模组边界 |
| [玩法-减法清单.md](docs/DOCS自制版/玩法-减法清单.md) | 保留 / 原型 / 砍掉 三档收敛 + 红线 + 开发纪律 |
| [玩法决策.md](docs/DOCS自制版/玩法决策.md) | 玩法方向与引擎通用原语（能源三态 / 结构物理 / 流体气体） |
| [世界生成数据驱动架构决策.md](docs/DOCS自制版/世界生成数据驱动架构决策.md) | 世界生成三层职责（模组 / 数据 / 引擎）+ 五步管线 |
| [流式加载与驻留架构决策.md](docs/DOCS自制版/流式加载与驻留架构决策.md) | 三轴独立 / 租约三原语 / WorldManager 复合体 |
| [模型格式决策.md](docs/DOCS自制版/模型格式决策.md) | 数据驱动方块模型格式（ModelDef / JSON / `#变量展开`） |
| [流体与气体系统设计.md](docs/DOCS自制版/流体与气体系统设计.md) · [风系统设计.md](docs/DOCS自制版/风系统设计.md) | 流体/气体与风场三层设计 |
| [MC架构借鉴.md](docs/DOCS自制版/MC架构借鉴.md) | MC Java 架构借鉴点与反模式 |
| [docs/references/minetest-chunk-mesh-analysis.md](docs/references/minetest-chunk-mesh-analysis.md) | Minetest 区块存储与 Mesh/LOD 参考分析 |
| [docs/legacy/沙盒引擎架构设计-历史蓝图.md](docs/legacy/沙盒引擎架构设计-历史蓝图.md) | 早期三轨蓝图（已过时，保留远期设计参考） |

> 26 单元的逐文件通读记录（`docs/通读记录/`）是作者的本地学习笔记，**不入库**；其中的计划骨架见 [docs/ENGINE_READTHROUGH.md](docs/ENGINE_READTHROUGH.md)。


## 构建

1. **VS2026** → 文件 → 打开 → 文件夹 → 选中本目录（含 `CMakeLists.txt` 这层）
2. 顶部配置下拉选 `x64-Debug`（或 Release），生成并运行 `sandbox_engine`

命令行：

```
cmake --build E:\MinecraftGame2\build --config Debug
# exe: E:\MinecraftGame2\build\Debug\sandbox_engine.exe
# 注意：运行工作目录需切到 exe 所在目录；assets/ 与 data/ 已由 CMake copy_directory 拷贝到 exe 旁
# （models/ textures/ 与 worldgen/presets/ 的资源相对路径都从 exe 目录解析）
```

依赖（不入库）：LunarG Vulkan SDK（通过 `VULKAN_SDK` 环境变量定位）+ 仓库内置 third_party（GLM / GLFW / RapidJSON / stb_image / spdlog，许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)）。

> **Jolt Physics 源码未入库**（`third_party/JoltPhysics/`，34.7MB / 1250 文件，已列入 `.gitignore`）。
> 当前物理求解器已下线，接入 Jolt 时再决定 vendor（直接入库）还是 submodule。

> **首次运行前**：先跑一次 `tools\gen_font_atlas.ps1` 生成 HUD 字体图集
> （`assets/textures/font/ascii.png`，160×96，依赖本机 Consolas 字体）。
> 该图集所在的 `assets/textures/` 被 `.gitignore` 整片忽略，clone 后不会有这张图；
> 缺它只会让 HUD 浮层初始化失败，不影响引擎主链路。

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

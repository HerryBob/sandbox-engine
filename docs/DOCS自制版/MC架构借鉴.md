# MC 架构借鉴

>状态：定案 2026-10-31　·　1a 落地核对 2026-11-04　·　关联：《世界生成数据驱动架构决策.md》《流式加载与驻留架构决策.md》
>决策背景：自研 C++20/Vulkan 体素沙盒引擎，世界生成器（1a）开工前，系统梳理 Minecraft Java 版代码架构对我们引擎的借鉴点与反模式

## 前提

>MC Java 版代码闭源；社区通过反混淆映射（Yarn/MCP）对架构有系统研究
>我们借鉴的是**架构思想**，不是代码（C++/Vulkan 与 Java 栈完全不同）；以下为社区公认事实

## 值得学 · 生成管线组（与 1a 最相关）

| MC 概念 | 我们的对应 | 状态 |
|---|---|---|
| `ChunkGenerator`（抽象生成器接口） | `WorldGenerator` | ✓ 已对齐：引擎不绑定具体地形 |
| `DensityFunction`（函数对象 + JSON 可序列化） | `NoiseSource` | ✓ 同构：输入坐标 → 输出数值；JSON 只是实例化通道 |
| `ChunkStatus`（生成分阶段：structure start → noise → features → full） | `WorldGenStage`（1a 暂内联） | ○ 单阶段未抽接口；出现装饰/洞穴第二阶段时再引入 |
| `ProtoChunk`（生成中间态）vs `LevelChunk`（就绪态） | 1b：`GenResult` 只在整列算完后才入队 | ✅ 已由架构规避：业务线程只能拿到完整 `ColumnBlocks`，半成品永远不会进 `World` |
| 分阶段生成 | 先地形后装饰 | ○ 后续（等装饰/洞穴阶段） |

## 值得学 · 数据组织组

| MC 概念 | 我们的对应 | 状态 |
|---|---|---|
| `Registry` + `ResourceLocation`（命名空间:路径） | 注册表 + 将来 namespace | ✓ 方向一致 |
| 双包 assets / data（资源包 vs 数据包） | 已对齐（资源双目录定案） | ✓ 已落地规划 |
| `BlockState`（16 位编码的属性组合） | 未做 | ▷ 记清单：红石/结构运动时需要，现在不做 |

## 值得学 · 存储组（记入未来优化清单）

>Palette（调色板）：16³ chunk 不用满位宽存方块，按实际出现种类用 1~8bit 变位宽存储
>我们目前 16³ × 1byte = 4KB/chunk，很便宜 —— **等内存成为瓶颈再上**（性能优化不提前，触发器纪律）

## 值得学 · 线程组（MC 的教训 = 我们的优势）

>MC Java 版生成/保存长期跑主线程（1.16 后才并行生成），卡顿闻名
>我们四轨（业务 25TPS + 独立渲染 + 后台生成线程）天生规避；1b 已落地后台生成线程 = "生成异步化"的我们的版本

## 不要学（反模式）

| 反模式 | 我们的对策 |
|---|---|
| 超大类（ServerLevel / LevelChunk 几千行） | 按域拆文件（src/worldgen/ 一个子系统一个目录） |
| 主线程阻塞生成 | 任何 IO/生成不进 25 TPS 主线；1b 已落地后台生成线程 |
| Mixin 字节码 hack（Fabric 生态不得已） | C++ 注册表 + 接口注入，模组路线天然更干净 |

## 对我们现在的直接落点

>已对齐：NoiseSource ≈ DensityFunction（函数对象设计：构造参数决定行为）；WorldGenStage ≈ ChunkStatus 阶段；src/worldgen/ ≈ 按域分层
>已落地（1b）：后台生成线程 + SPSC 双队列；ProtoChunk 语义用"整列算完才入队"规避（半成品不进 World）
>后续可落：chunk 生成分级（先地形后装饰，玩家靠近再补细节）
>记清单（触发器到时再动）：Palette 调色板 / BlockState 16bit 编码 / pack 栈资源合并（模组化时）

## 1a 落地核对（2026-11-04）

| 借鉴点 | 1a 实际落地 | 结论 |
|---|---|---|
| `DensityFunction` ≈ `NoiseSource` | 接口 + `NoiseRegistry` 工厂表 + `NoiseParams` 数值信封（引擎零 type 硬编码） | ✓ 已落地 |
| `ChunkGenerator` ≈ `WorldGenerator` | 五步管线（预设校验 → 地层解析 → 连续性校验 → 逐列采样 → 批量写入） | ✓ 已落地 |
| `ChunkStatus` ≈ `WorldGenStage` | 单阶段内联在 WorldGenerator 中，未抽接口 | ○ 延后到 1b |
| `ProtoChunk` 生成中间态 | 1a 一次性生成完整区块，无中间态 | ○ 1b 异步生成时再引入 |
| 双包 assets / data | `assets/models`（表现）+ `data/worldgen/presets`（逻辑）已分开 | ✓ 已落地 |
| Palette 调色板 | 仍 1 byte/块（4KB/chunk） | ▷ 记清单，内存成瓶颈再上 |
| `BlockState` 属性组合编码 | 未做：草方块只有 BlockType 一种状态（无朝向/生长阶段） | ▷ 红石/结构运动需要时再上 |

>确定性核对：MC 靠 seed + 噪声函数纯函数性保证"同 seed 同世界"；我们同样以 `sample(x,z,seed)` 纯函数为命脉——1a 实测同 seed 两次生成逐位一致（76447 块 / 28..36），改 seed 后结果变化。

## 来源

>Minecraft Wiki（机制文档）、Fabric 生态文档（Yarn 反混淆映射研究）——社区公认事实来源
>https://minecraft.wiki/　·　https://docs.fabricmc.net/

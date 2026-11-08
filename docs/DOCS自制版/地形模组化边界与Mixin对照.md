# 地形模组化边界与 Mixin 对照

>状态：**方案待拍板**（编号 D27 候选，未并入总表）· 2026-11-07
>关联：《世界生成数据驱动架构决策.md》（当前三层职责）、《模组架构决策.md》（侵入式策略）、《架构决策-注册表机制.md》（注册表能力）

## 一、这份文档解决三个问题

1. 地形生成能不能交给 DLL 模组？如果能，交到哪一层为止？
2. 跨 DLL 传递一棵"密度函数图"（density function graph）该用什么数据结构？
3. MC 的地形模组靠 Mixin（字节码注入）做到了什么？为什么本引擎做不到，也不该做？

结论先行：**模组能换"用什么材料"，不能换"怎么砌墙"。**

---

## 二、MC 侧实测：Mixin 在地形模组里到底改了什么

### 2.1 样本：Tectonic

仓库 `github.com/Apollounknowndev/tectonic`，分支 `rewrite-squared`。根目录存在 `mappings/21.1.tiny`——Mixin 需要的混淆映射（tiny 格式），本身即"用了字节码注入"的标志。

`src/common/main/java/dev/worldgen/tectonic/mixin/` 下共 **12 个类**，客户端另有 `client/WorldCreationUiStateMixin`：

| 类 | 体积 | 改动对象 | 证据级别 |
|---|---|---|---|
| `NoiseBasedChunkGeneratorMixin` | 2397 B | 替换私有字段 `globalFluidPicker` | 已读源码 |
| `NoisesMixin` | 994 B | `@ModifyArg` 改噪声种子标识符 | 已读源码 |
| `WorldCarverMixin` | 582 B | 注入体**整体被注释**，只留 TODO | 已读源码 |
| `BlendingDataMixin` | 1233 B | 新旧区块混合（推断） | 仅类名 |
| `HeightmapMixin` | 501 B | 高度图缓存（推断） | 仅类名 |
| `ChunkAccessMixin` | 833 B | 区块高度范围（推断） | 仅类名 |
| `IOWorkerMixin` | 779 B | 区块存档 IO（推断） | 仅类名 |
| `NoiseSettingsAccessor` / `DimensionTypeAccessor` | 427 / 515 B | `@Accessor` 读私有字段 | 仅类名 |
| `BiomeMixin` / `StructurePieceMixin` / `TemperatureModifierMixin` | 683 / 1136 / 699 B | 生物群系、结构、温度（推断） | 仅类名 |

标注"推断"的 9 个类未逐个读取源码，引用时须保留这一限定。

### 2.2 三个已核实的行为

**① 修正尺度不匹配：岩浆层**

`NoiseBasedChunkGeneratorMixin` 在构造函数 `@At("RETURN")` 处把私有字段 `globalFluidPicker`（流体拾取器）整个替换：岩浆水面从原版固定值改为 `config.heightLimits.minY + 10`。

原因是 Tectonic 改了世界高度范围，原版岩浆层的固定位置随之失效。**这不是新增功能，是修原版失效的假设。**

**② 种子对齐**

`NoisesMixin` 用 `@ModifyArg` 拦截 `Noises.instantiate` 里的 `PositionalRandomFactory.fromHashOf(Identifier)`，把 `tectonic:parameter/xxx` 映射回 `minecraft:xxx`。目的是让自定义噪声球与原版使用同一 seed——否则"大陆性"在密度函数系统和生物群系系统里会算出不同值，地形与生物群系对不上。

**③ 一次失败的尝试**

`WorldCarverMixin` 的注入体被整段注释，只剩 `// TODO: Look into this again with some sort of cave_entrances config option`。洞穴雕刻改造尝试过，放弃了。

### 2.3 关键结论：Mixin 在这里是"胶水"，不是主体

Tectonic 的资源侧同时提供 `resourcepacks/tectonic/data/minecraft/worldgen/density_function/overworld/noise_router/` 下 **15 个 JSON**（`barrier` / `continents` / `depth` / `erosion` / `final_density` / `fluid_level_floodedness` / `fluid_level_spread` / `initial_density_without_jaggedness` / `lava` / `ridges` / `temperature` / `vegetation` / `vein_gap` / `vein_ridged` / `vein_toggle`），即整个噪声路由（noise router）被替换。

按体积看复杂度分布：`continents`(96 B) / `erosion`(98 B) / `ridges`(97 B) / `temperature`(97 B) / `vegetation`(96 B) 都只是噪声引用包装——**换的是噪声实例，不是结构**；真正复杂的是 `final_density`(1751 B)、`initial_density_without_jaggedness`(1643 B)、`vein_ridged`(1158 B)。

**地形形状靠数据包，Mixin 只补数据包够不到的地方。** 这是理解 MC 地形模组最重要的一条。

### 2.4 Mixin 的五类动机

| 动机 | 典型注入点 | 例子 |
|---|---|---|
| 修正参数不匹配 | `NoiseBasedChunkGenerator.<init>`、`Noises.instantiate` | 岩浆层、种子 |
| 接管生成流程 | `fillFromNoise` 的 `@At("HEAD")` | 动态海平面系统 |
| 性能重写 | `NoiseChunk` 采样路径 | Noisium（不改形状只加速） |
| 存档/边界兼容 | `BlendingData`、`Heightmap`、`ChunkAccess` | 新地形接原版存档 |
| 界面/配置 | `WorldCreationUiState` | 世界类型与配置入口 |

---

## 三、为什么 C++ 侧没有等价物

| 维度 | Java（Minecraft） | C++（本引擎） |
|---|---|---|
| 类加载期 | 有，可重写字节码 | 无，编译即固化机器码 |
| 事后插入任意方法 | Mixin 支持 | 只能 detour/hook 改机器码 |
| 稳定性 | 受混淆映射约束，但可控 | Debug/Release 优化差异、杀软拦截、不可维护 |
| 结论 | 侵入可行 | **扩展口必须设计期预留** |

推论：**Mixin 的存在，是"原版数据化做得不彻底"的症状。** 岩浆层、海平面、高度范围是硬编码常量，模组才需要注入。本引擎如果把这类常量一开始就做成 preset 字段，就永远不需要"修岩浆层"这类补丁。

---

## 四、五层归属：地形交到哪一层为止

| 层 | 内容 | 归属 | 理由 |
|---|---|---|---|
| L0 噪声算法 | perlin / fbm / worley | ✅ 已开放 | 纯函数（坐标 → float），跨 DLL 只需 `void*` + 函数指针 |
| L1 噪声实例与参数 | scale / octaves / seed | ✅ 已开放 | `NoiseRegistry` + preset JSON |
| L2 形状节点 | 自定义噪声节点、样条节点 | ⚠️ 应扩展 | 需 3D 采样签名；是"词汇"，可叠加 |
| L3 组合子 | add / mul / min / max / spline / shift | ❌ 不开放 | 这是引擎的"语法"，开放等于允许模组改引擎 |
| L4 规则层 | 表层规则、含水层、边界约束 | ⚠️ 只给参数，不给逻辑 | 规则决定"合法不合法"，写错整片世界被挖穿 |
| L5 生成主流程 | 调度、线程模型、批量节奏、产物契约 | ❌ 不开放 | 改 `ColumnBlocks` 契约、`worldgen-stream` 线程假设、`kMaxApplyPerTick` 全要跟改 |

类比：**引擎是编译器，模组是指令集扩展**——编译器决定有哪些语句类型、语句怎么执行（这是不变量），扩展只提供新的叶子指令（词汇）。

---

## 五、跨 DLL 的地形图怎么表达

### 5.1 为什么不能用对象树

`std::shared_ptr<DensityNode>` 由 DLL 创建、引擎持有，有三处未定义行为（undefined behavior, UB）：

1. **控制块（control block）在 DLL 堆上**，引擎用引擎的分配器释放——不同模块的 CRT 堆不保证一致；
2. **虚表指针（vtable pointer）指向 DLL 代码段**，`FreeLibrary` 后即悬垂；
3. **树的生命周期跨两次 `FreeLibrary`**。

### 5.2 正确形态：扁平 POD 节点数组 + 索引连接

```cpp
struct DensityNodeDesc {
    std::uint8_t  kind;
    std::uint8_t  inputA;
    std::uint8_t  inputB;
    std::uint8_t  reserved;
    std::uint32_t noiseTypeHash;
    float         param[4];
};

struct DensityGraphDesc {
    const DensityNodeDesc* nodes;
    std::uint32_t          nodeCount;
    std::uint32_t          rootIndex;
};
```

好处：

- **天然可序列化**：同一份描述既能从 DLL 传进来，也能从 JSON 读进来；
- **无指针、无虚表、无 STL**；
- **引擎可校验**：节点数上限、索引越界、环检测（防 A→B→A 死循环）；
- **卸载可清理**：模组注册的节点类型走 `eraseByOwner`，图里引用处由引擎自己断开。

### 5.3 关键设计

**DLL 只注册"节点类型"，不注册"整棵树"。** 树由引擎按 graph / preset 装配。这样模组卸载后，引擎手上的树结构仍然自洽。

---

## 六、规则层：参数可以给，逻辑不该给

理由：规则决定地形合不合法。模组写错一个掩码（mask），结果就是整片世界被挖穿或玩家掉出地图，而错误表现会指向引擎而不是某个模组。

折中：引擎提供**有限原语枚举** `SurfaceGrass / Sand / Snow / Stone / Layered`，模组只能"选与配"，不能"写解释器"。

---

## 七、预留扩展入口清单

| # | 接口 | 位置 | 现状 | 何时需要 |
|---|---|---|---|---|
| 1 | `NoiseProviderDef.sample3D` | `modapi/noise_api.h` | ❌ 只有 2D `sample(self,x,z,seed)` | 上 3D 密度场时**必须**新增字段（按追加纪律，不改签名），`kEngineApiVersion` 3 → 4 |
| 2 | 节点类型注册 | `NoiseRegistry` 扩展 | ⚠️ 只有噪声类型 | L2 落地时 |
| 3 | `DensityNodeDesc` / `DensityGraphDesc` | `modapi/terrain_api.h`（新文件） | ❌ 不存在 | 允许模组注入形状节点时 |
| 4 | 表层规则原语枚举 | preset JSON | ⚠️ 现在只有 strata | 3D 改造阶段 3 |
| 5 | 含水层参数 | preset JSON | ❌ 不存在 | 阶段 3 之后 |
| 6 | 世界高度范围 `minY` / `maxY` | `WorldPreset` | ⚠️ 现为编译期常量 `kGenMinY` / `kGenMaxY` | 需要纵向扩展地形时 |
| 7 | 岩浆层高度 `lavaLevel` | `WorldPreset` | ❌ 不存在 | **越早做越好**——MC 就是漏了这个才要 Mixin |

第 6、7 项是"把 Mixin 想改的常量提前数据化"的直接落地。

---

## 八、三个风险与对策

| 风险 | 表现 | 对策 |
|---|---|---|
| 卸载悬垂 | 模组卸载后，图里仍引用其节点类型 | 复用 A2 的 `eraseByOwner`；卸载时引擎先丢弃引用该类型的图 |
| 输出非法 | 模组返回 NaN / Inf / 极大值，导致全实心或全空 | 引擎侧 `std::isfinite` 校验 + clamp 到 `[-1e6, 1e6]`，非法记 Warn 当 0 |
| 存档不兼容 | 世界由某模组生成，模组缺失后地形对不上 | 世界元数据记录"生成时用了哪些模组 + 版本"，缺失时警告而非静默重生成（本引擎暂无存档系统，记待办） |

---

## 九、明确不做

- **不做 Mixin 等价物**：不做运行时机器码改写 / detour；
- **不开放组合子**：L3 归引擎；
- **不开放生成主流程**：L5 归引擎；
- **不让模组写规则解释器**：规则层只给原语枚举。

---

## 十、待拍板

| # | 问题 | 选项 |
|---|---|---|
| 1 | 地形要不要允许模组改 | 要 → 走 JSON 化 DAG；不要 → 纯 C++ 组合子（更快更安全） |
| 2 | DAG 表达形式 | A 纯 C++ 组合子 / B 纯 JSON / **C 混合（C++ 组合子 + JSON 描述装配，倾向此项）** |
| 3 | 3D 密度临时缓冲 | A 函数内 `std::vector` / **B 放进 `ColumnBlocks` 随 `shared_ptr` 传递（倾向）** / C 线程局部缓冲 |
| 4 | 是否进入实际改造 | 阶段 0（抽象判据，验收：跑测数字逐位不变）/ 阶段 0+1（抽象 + 洞穴） |

---

## 附录：来源

- Tectonic 仓库（分支 `rewrite-squared`）：https://github.com/Apollounknowndev/tectonic
- Tectonic Mixin 目录：https://github.com/Apollounknowndev/tectonic/tree/rewrite-squared/src/common/main/java/dev/worldgen/tectonic/mixin
- `NoiseBasedChunkGeneratorMixin.java` 原文：https://raw.githubusercontent.com/Apollounknowndev/tectonic/rewrite-squared/src/common/main/java/dev/worldgen/tectonic/mixin/NoiseBasedChunkGeneratorMixin.java
- `NoisesMixin.java` 原文：https://raw.githubusercontent.com/Apollounknowndev/tectonic/rewrite-squared/src/common/main/java/dev/worldgen/tectonic/mixin/NoisesMixin.java
- `fillFromNoise` 的 HEAD 注入案例（Forge 论坛，动态海平面）：https://forums.minecraftforge.net/topic/160438-forge-1201-world-generation-architecture-review-dynamic-rising-sea-level-system/
- TerrainForged（独立生成器接回原版，保留原版洞穴/含水层/矿/结构）：https://www.curseforge.com/minecraft/mc-mods/terrainforged
- Terrain Recast（纯数据包覆盖 dimension / noise_settings / density_function / material_rule）：https://www.curseforge.com/minecraft/mc-mods/terrain-recast
- GeoTectonic（替换洞穴系统并为 Tectonic / Terralith / YUNG's Better Caves 带覆盖包）：https://www.nexusmods.com/minecraft/mods/1289
- Fabric 官方教程（NoiseChunkGenerator 与 DensityFunction / SurfaceRule 的定制能力）：https://www.fabricmc.net/wiki/tutorial:chunkgenerator
- MC 密度函数算子全表：https://minecraft.fandom.com/zh/wiki/密度函数

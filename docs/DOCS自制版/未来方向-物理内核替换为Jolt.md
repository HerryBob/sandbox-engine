# 未来方向：物理内核替换为 Jolt

> **状态**：已定方向，**未实装**（2026-11-14 讨论定案）
> **当前动作**：先把 `src/` 通读完成（`docs/通读记录/`），掌握更深之后再动
> **目标效果**：至少达到 Minecraft 模组「机械动力航空学」（Create: Aeronautics）

---

## 一、决策与现状

| 项 | 值 |
|---|---|
| 选定内核 | **Jolt Physics 5.6** |
| 位置 | `third_party/JoltPhysics`（**完整上游仓库 34.7 MB / 1250 文件**） |
| 实际需要 | 只有 `Jolt/` = **4.4 MB**；`Assets`(21 MB)/`Docs`(5.5 MB)/`Samples`/`UnitTests`/`PerformanceTest`/`JoltViewer`/`Build` 可删 |
| 构建接入 | **未接入**（`CMakeLists.txt` 无引用，`src/` 零引用） |
| 上手文件 | `third_party/JoltPhysics/HelloWorld/HelloWorld.cpp`（17 KB，接入全流程） |

---

## 二、为什么换（不是"Jolt 更专业"）

现在的物理有一个**架构级天花板**，不是缺功能：

- Dynamic 刚体碰撞只查 `colliders_`（静态方块）
- 玩家 Kinematic 推进也只查 `colliders_`
- `contactPairs_` 的键是"刚体 id → 静态方块坐标"

⇒ **`bodies_` 之间没有任何交互**。两个直接后果：

1. 玩家**站不上**飞船只（飞船对他不存在）
2. 两个复合体**不会互相碰撞**（多飞船/撞船全部穿过去）

这是"静态格点集合"与"刚体列表"两套世界表示之间没有统一空间查询的问题，手工补等于重写一遍宽相+窄相+约束求解。

---

## 三、关键认知：Jolt 只覆盖一层

```
Create: Aeronautics ≈ Sable
Sable = Rapier（刚体物理）+ 六层自研
```

| 层 | 内容 | 谁提供 | 项目现状 |
|---|---|---|---|
| **L0 刚体物理** | body/碰撞/约束/CCD | **Jolt** | 手搓（x-only 回退） |
| **L1 子级(sub-level)** | 独立坐标系 + 方块/实体归属 + 双向同步 | 自研 | 雏形（`contraptionId` + 局部坐标 + pack/unpack） |
| **L2 属性驱动** | 每方块质量/惯量/体积/摩擦/弹性 | 自研 | `BlockInfo` 有 `mass/friction/solid`，**无人读** |
| **L3 气动/浮力** | 压力×体积、阻力、升力、三维重力 | 自研 | 无 |
| **L4 实体交互** | 站在移动体上、相对速度传递 | 自研（`CharacterVirtual` 帮一半） | 无 |
| **L5 动态改造** | 建造/破坏改变质量分布与重心 | 自研（`MutableCompoundShape` 帮一半） | 只有 AABB 重算 |
| **L6 渲染/存档/联机** | 子级渲染、插值、持久化 | 自研 | 刚体网格，无子级渲染 |

**Jolt 大约占整个工程量的 15–20%：必要，但不充分。**

---

## 四、Sable 的模型摘要（来自 `Allmd/…/sable-main/wiki/`）

### 方块属性（**按 block-state**存，datapack）

`sable:mass`(默认 1.0 kpg) / `sable:inertia`(默认 `[1/6,1/6,1/6]`) / `sable:volume`(浮力用) /
`sable:restitution` / `sable:friction` / `sable:fragile` / `sable:floating_material`

支持 `overrides: { "lit=true": { "sable:mass": 3.0 } }` —— **同一方块不同状态可以不同质量**。

### 维度属性

`base_gravity`（**三维向量**，默认 `[0,-11,0]`）/ `base_pressure` / `pressure_function`（按高度分段的贝塞尔曲线）/
`universal_drag`（0.09）/ `magnetic_north`

⇒ Aeronautics 的核心体验不是"刚体会掉下来"，而是**大气**：质量/体积/压力/阻力/浮力共同决定"什么能飞"。

---

## 五、三个被低估的前置依赖

1. **需要 block state，而项目现在没有。**
   `BlockType` 只有 `Air = 0`；`PaletteEntry` 注释写着"block state 落地后改存全局 state id"，**位置留了，维度没有**。
   没有 state 就做不了"提灯点亮后更轻""活塞伸出后更重"这类。

2. **每方块质量不是白送的。**
   Jolt 的 `CompoundShape` 子形状不带质量，按体积×均匀密度算。要"石头重/木头轻/气球几乎无质量"，
   必须自己累加 `MassProperties`（含平行轴定理）再用 `mMassPropertiesOverride` 覆盖。

3. **子级不只是物理问题。**
   它是一套**坐标空间**：方块/实体/block-entity 都在里面，渲染、碰撞、存档都要认两套坐标。
   现在的 `contraptionId` 只覆盖"刚体里的方块列表"，缺：玩家归属、子级内方块如何在方块表里索引、
   是否要在子级坐标系建网格（现在是世界坐标烘进顶点 + push constant 平移）。

---

## 六、v1 范围（收窄后）

| 决定 | 理由 |
|---|---|
| 地形：每 chunk 一个 **`StaticCompoundShape`** | 先接受"编辑时重建整个 chunk"的代价，不做 Mutable 混合 |
| 步长：**25 Hz + `CollisionSteps = 2`** | 外部契约不破，插值 / `PlayerBodyState.tickDurationNs` 不用碰 |
| 玩家：**`CharacterVirtual`** | 顺带拿到"站在移动甲板上"，手搓方案补不了 |
| 复合体：**`MutableCompoundShape`** | 正好对上"飞船改造 = 增删子形状" |
| 刚体互碰：**不专门调** | Jolt 自带，不在 v1 验收范围 |
| 门面：**保留 `PhysicsWorld`** | 公开接口不变，桥/业务零改动；**硬约束：不得泄漏任何 Jolt 类型到桥** |

**v1 验收只有三条**：站得住、放/破坏方块地形跟着变、打包→飞→解包落点一致。

> **⚠️ 2026-11-23 修正（来自《七大维度与一大底座设计/09-模组化边界》§九 第 1 条）：**
> 上面「复合体：`MutableCompoundShape`」指的是**一个刚体内部的子形状**。
> 刚体的组织方式已决定采用 **多刚体 + 约束表**（一个结构 → N 个 `PhysicsBody` + `PhysicsConstraint` 表），
> 因此 `PhysicsState` 从第一版就按这个结构建，1.0 只用一个刚体、约束表为空。
> 这样 07（约束/关节）上线时只需加约束类型，不改数据结构；`contraptionId` 的语义随之为**结构 id**。

---

## 七、三个暂缓项（已明确先不管）

1. 重建一个 3000 盒子 chunk 的 BVH 耗时
2. 全 `StaticCompoundShape` 在连续建造时的尖刺
3. 刚体与刚体之间的交互表现

---

## 八、建议顺序

```
通读完成（当前）
  → 补 block state 的【数据结构】（不动渲染、不动协议）
  → 换 Jolt（v1 范围）
  → L2 质量属性 / L4 实体交互
  → L3 气动浮力（档 2）
```

**理由**：state 决定 `PaletteEntry` 的语义，而物理适配层要读它算质量 —— 两者同源，一起做省一轮返工。

---

## 九、通读时的重点调整

| 单元 | 调整 |
|---|---|
| **U02**（ChunkData/PaletteEntry） | 升为**必须吃透**：block state 将在这里落地 |
| **U04**（World/BlockActions） | 升为**必须吃透**：哪些变更要通知物理、方块如何归属子级 |
| **U07 / U08**（物理） | **改略读**：只记"必须保住什么"（指令/回执语义、时间推进策略、`colliders_` 生命周期），求解器反正要换 |
| **U09**（输入） | 记"意图与位置的分界"，`CharacterVirtual` 接这里 |
| **U20**（帧循环） | 记 `bodyPos_` double→float 那处（子级远离原点的精度落点） |
| **U12**（流式） | 将来"子级内的 chunk"要复用这套请求/淘汰 |

---

## 十、Jolt 阅读路径（按需要排序）

1. `HelloWorld/HelloWorld.cpp` —— 一个文件串完接入全流程
2. `Jolt/Physics/Body/BodyInterface.h` + `BodyCreationSettings.h` + `MotionType.h`
3. `Jolt/Physics/Collision/Shape/`：`BoxShape` / `StaticCompoundShape` / `MutableCompoundShape` / `CompoundShape`（父类，看 `GetSubShapeID`）
4. `Jolt/Physics/Character/CharacterVirtual.h`
5. `Jolt/Physics/Collision/ContactListener.h`
6. `Docs/Architecture.md`（唯一值得读的文档：layer / broadphase / 步进结构）

**不要碰**：`Vehicle/`、`Ragdoll/`、`SoftBody/`、`Hair/`、`Constraints/`（等要做关节再说），以及 NarrowPhase/EPA 实现细节。
遇到 API 用法问题，翻 `UnitTests/Physics/` 比读 `Samples/` 快。

---

## 十一、明确不做

- ❌ 不为了 Jolt 改 tps（25 Hz 是插值、桥节奏、文档口径的共同基准）
- ❌ 不在通读未完成时动 `PhysicsWorld.cpp`
- ❌ 不把"换内核"和"线程类化"（见 `未来方向-线程拆分为独立类.md`）合并做

---

## 十二、后续：物理 → 渲染直连队列（已定，暂不做）

> 定于 2026-11-17。**结论：该加，但加在 Jolt S2 之后**，现在只预留一个字段。

### 现状：三条渲染数据路径，各走各的

| 数据 | 路径 | 安全靠什么 |
|---|---|---|
| **区块** | 渲染线程**直接调** `BusinessWorld::renderSnapshot / chunkSnapshot` | `World` 里的 `blocksMutex_` |
| **刚体** | 物理 → `resultQueue_`(B2) → **业务线程中转** → `bodyStructureQueue_`/`bodyPoseQueue_`(B6) → 渲染 | 物理侧无锁，只能走队列 |
| **玩家模型** | 渲染线程**直读** `PlayerBodyState`（原子 + seqlock 插值） | 原子 + 版本号 |

规律：**有锁/有原子的能直读，没有的只能走队列。**

### 为什么要加直连（第三条最硬）

1. **延迟**：刚体位置现在绕业务线程，物理算完 → 下个 tick 业务转发 → 渲染下一帧画，**1~2 帧延迟**。
2. **解耦**：业务线程卡住（流式、全量快照）时刚体渲染跟着停 —— 两者本来没有依赖。
3. **时间基准必须一致**（关键）：
   - 玩家模型走 `PlayerBodyState`：**有插值**，60fps 平滑
   - 刚体位姿走业务中转：**无插值**，40ms 一跳
   - 现在没暴露，因为**玩家站不到飞船上**（`bodies_` 之间不交互）
   - **Jolt 之后玩家能站上去** → 玩家平滑地悬在飞船上方，偶尔穿进去
   - 要一致，就得让刚体位姿也走"直连 + 插值"

### 加法的代价：帧标记

直连会引入两个"世界时间"（地形的业务时间 / 刚体的物理时间），不一定来自同一 tick。
平时无事（地形几乎不动），但**移动地形**（活塞、升降平台）会撕裂 —— 新飞船站在旧地形上。

→ 队列里必须带 **`simTick`**，渲染端记住"当前地形的 tick"，对不上就用插值。

**所以直连的真正代价不是多一条队列，是引入"帧标记"。**

### 建议形态：按语义分流，不是按"刚体 vs 区块"

| 数据 | 走哪条路 | 理由 |
|---|---|---|
| **刚体位姿** | ✅ **物理 → 渲染直连**（新队列，状态语义，满了丢） | 高频、只要最新、延迟敏感 |
| **刚体结构事件**（建/拆网格） | ❌ 仍走业务 | 低频、事件语义、业务要登记"谁拥有这个刚体" |
| **区块** | 保持直读（短期）；长期应改"快照 + 版本号" | 有锁，暂时够用 |
| **玩家** | 保持直读 `PlayerBodyState` | 已经是原子 + seqlock |

**拆分点是"状态 vs 事件"，不是"刚体 vs 区块"。**

### 触发条件（写具体）

> 当你第一次让 `CharacterVirtual` 站在 `MutableCompoundShape` 上，
> 并且发现**玩家相对飞船在抖** —— 那就是该加队列的信号。

### 现在只做一件事

```cpp
struct BodyPose {
    int contraptionId;
    double x, y, z;
    // 将来加：四元数旋转
    // 将来加：long long simTick;   ← 为直连队列准备（现在加字段几乎不要钱）
};
```

### 不做

- ❌ 不在 Jolt 迁移期间加（一次只改一件事，否则出问题分不清是物理还是架构）
- ❌ 不把它和"线程类化"合并做
- ❌ 不现在就把区块也改成队列（等联机/移动地形再说）

---

## 十三、刚体运动类型与睡眠机制（2026-11-18 记录，**暂不实现**）

> 起因：讨论"空岛 / 大型飞行构造在静止时怎么不耗 CPU"。
> 结论：**目前不做状态切换** —— 1.0 用 `Dynamic + gravityFactor` 就够。本节只记录源码事实与将来方案。
> 参考：`Allmd/Jolt-空岛物理方案-讨论总结.md`（另一份调研，核心结论与此一致）。

### 13.1 睡眠机制（源码核实）

| 事实 | 位置 |
|---|---|
| 判据是**世界坐标位移**：0.5 秒内三个测试点移动 ≤ **15mm** | `Body.cpp:145–184`；`max_movement = 0.03 × 0.5`（`PhysicsSystem.cpp:2526`） |
| 三个测试点：**质心 / 质心+最大包围盒轴 / 质心+第二大轴**；球**只扩不缩** | `Body.h:432`、`Body.cpp:160–181` |
| **岛内所有 body 都能睡，整岛才睡**（`all_can_sleep &=`），且只在**最后一个子步**检查 | `PhysicsSystem.cpp:2511–2548` |
| **入眠时速度被清零**（注释 "Reset velocity"） | `BodyManager.cpp:559–561` |
| **力每步清零**（`ResetForce/ResetTorque`）→ 悬停必须每帧重新施力 | `PhysicsSystem.cpp:2539–2542` |
| 默认 `mTimeBeforeSleep = 0.5`、`mPointVelocitySleepThreshold = 0.03`（**全局**） | `PhysicsSettings.h:95/102` |

### 13.2 四条唤醒路径（只有一条有阈值）

| 路径 | 有阈值吗 | 可调吗 |
|---|---|---|
| `SetLinearVelocity` / `SetLinearAndAngularVelocity` / `MoveKinematic` | ✅ `IsNearZero()` = **1e-6 m/s** | ❌ 硬编码（但可选择不调） |
| `AddImpulse` | ❌ 无条件唤醒 | ❌（但可选择不调） |
| `AddForce(Activate)` | ❌ 无 | ✅ **改用 `DontActivate` 就不唤醒**（睡着时力被丢弃） |
| **接触** | ❌ **完全没有** | ❌ **不可控** |

**关键**：`ContactConstraintManager::CreateConstraint` 只对 `IsDynamic()` 的 body 收集"待唤醒"。
→ **Kinematic 不会被接触唤醒**，但**照常收得到接触事件**（回调在更早的位置）。

### 13.3 三种运动类型的定位

| 类型 | 适合 | 关键限制 |
|---|---|---|
| **`Static`** | 地形 chunk、永久设施 | 切 Static 会**清零速度**（`Body.cpp:56–60`）→ 有速度断崖 |
| **`Dynamic`** | 一般刚体、被物理支配的东西 | 受重力/力/阻尼；会被接触唤醒 |
| **`Kinematic`** | 玩家、生物、自驱动构造、**冻结态** | 不受力、**无阻尼**（`MotionProperties.inl:131` 断言 Dynamic）、**不被接触唤醒** |

**切类型的前置条件**（`Body.cpp:42–43`）：
1. 建 body 时必须 `mAllowDynamicOrKinematic = true`，否则**永远切不了**
2. 切 Static 前必须 `DeactivateBody()`

### 13.4 力平衡悬停能不能睡？—— **几乎不能**

- 判据只看位移，理论上"力恰好抵消 → 不动 → 能睡"
- 但**力每步清零** → 必须每帧施力；用 `Activate` 施力 = 每帧唤醒
- 即使 `DontActivate`，任何控制回路都有抖动，而阈值只有 **15mm / 0.5s**
- **结论：`Dynamic + 力平衡` 与 `能睡眠` 基本互斥**

**替代方案（推荐，1.0 就用这个）**：

```cpp
bodyInterface.SetGravityFactor(id, 1.0f - lift / weight);
```

| lift / weight | factor | 效果 |
|---|---|---|
| 0 | 1.0 | 正常下落 |
| 0.8 | 0.2 | 缓慢下落 |
| 1.0 | 0.0 | 悬停 |
| 1.2 | **−0.2** | 缓慢上升（Jolt 不限制负值） |

**一个 float 表达净力，不用切类型、物理连续、自动感知撞击与过载。**

### 13.5 匀速运动与睡眠

| 事实 | 说明 |
|---|---|
| **匀速永远睡不着** | 睡眠是"零位移"，匀速必然有位移 |
| 强行让它睡会怎样 | 入眠清速度 → 它就不动了（与"匀速"矛盾） |
| 它**需不需要**睡 | **不需要** —— 移动中的空 island 在求解器里被直接跳过（`PhysicsSystem.cpp:1477`） |
| 真正的成本 | 不是"睡不睡"，是**它经过的地方有没有别的物体**（宽相 pair + 窄相） |

**Dynamic 要做匀速**必须同时：`mLinearDamping = 0`、`mAngularDamping = 0`、`mGravityFactor = 0`（默认 damping 0.05 会每帧减速）。

### 13.6 将来的方案（记录，**暂不实现**）

**三态状态机**（统一机制，但不统一状态集）：

| BodyState | MotionType | 速度 | 允许的物体 |
|---|---|---|---|
| `Immovable` | Static | 0 | 地形、永久设施 |
| `Frozen` | Kinematic | 0（睡着） | 大型构造 |
| `Driven` | Kinematic | 每 tick 由逻辑设 | 玩家、生物、玩家操控的船 |
| `Simulated` | Dynamic | 物理决定 | 大型构造（被撞后）、小道具 |

**为什么不能统一状态集**：小道具绝不该进 Kinematic（不会被推动 → 悬空卡住）；地形只能是 Static。

**迁移必须集中在一个 `MotionStateManager`**，且需要四个补丁：
① 相对阈值 `|ΔF|/weight`（1%~5%）② 滞后（进入 ε、退出 2ε）③ 最短停留时间 ④ **接触事件触发**（周期检查抓不到"被撞"）

**切换流程（Kinematic → Dynamic）**：
```
接触事件只记录（回调里禁止改物理状态）
  → Update 后算动量 Δv = j / M
  → 超阈值 → SetMotionType(Dynamic, Activate)
  → SetLinearAndAngularVelocity(Δv)   ← 必须补速度，否则"切了但不动"
```

**物理 LOD（比状态机更值得先做）**：

| 距离 | 做法 |
|---|---|
| 近 + 活跃 | Dynamic 全物理 |
| 近 + 静止 | Kinematic 冻结 |
| **远** | **`RemoveBody` 移除，逻辑移动；玩家靠近 `AddBody` 加回** |

### 13.7 为什么现在不做 + 什么时候再做

| 理由 | 说明 |
|---|---|
| 1.0 只有几艘演示船 | 用不上状态机 |
| `gravityFactor` 已能表达"升力" | 一个 float 解决 90% 需求 |
| 状态机需要接触事件 + 动量估计 + 滞后 | 是"另一套系统"，不是参数调整 |
| **触发条件** | ① 构造数量 > 20 ② 或 `Update()` 耗时 > 2ms ③ 或需要"过载掉落/升力失效"玩法 |

---

## 十四、接入准备（2026-11-23 记录）

### 决策

| # | 决策 | 结论 |
|---|---|---|
| **A** | block state 与 Jolt 的先后 | **block state 先行**（按 §八）。理由：state 决定 `PaletteEntry` 语义，物理适配层要按 state 取质量，两者同源，一起做省一轮返工 |
| **B** | 多刚体改造的时机 | **与 Jolt 一起改**（一次完成），但拆成独立一步：先改数据结构（手搓占位实现仍能跑），再接 Jolt |
| **C** | 精度方案 | **采用 Jolt 官方推荐：float 内核 + 世界原点偏移**（不启用 `DOUBLE_PRECISION`），详见下节 |

### 精度方案：float + 世界原点偏移（官方推荐，2026-11-23 定）

**结论**：Jolt 保持 `DOUBLE_PRECISION=OFF`（float 内核，默认配置 —— 性能与 ABI 都最稳），
引擎自己的世界坐标继续用 `double`，两者靠"每个结构一个 float 原点"桥接。

| 层 | 用什么 | 说明 |
|---|---|---|
| 引擎世界坐标 | `double` | `RigidBody.x/y/z`、渲染侧 `bodyPos_` 都是 double，保持不动 |
| Jolt 内部坐标 | `float`（相对原点） | 刚体位置的**有效位数全花在局部**，远处不抖 |
| 桥 | 每个结构一个 `float origin` | Jolt 坐标 = 世界坐标 − origin |

落地要点：

1. **喂进 Jolt 的位置必须是相对原点**：`BodyInterface::SetPosition(id, RVec3(x,y,z))`
   —— `DOUBLE_PRECISION=OFF` 时 `RVec3` 就是 `Vec3`，传绝对世界坐标等于把精度丢掉。
2. **移动原点（moving origin）**：结构整体走远时，重新定 origin（把所有刚体位置减去新原点后重设）。
   相机/渲染侧本来就在做 double→float 的局部化，物理侧对齐同一套做法即可。
3. **与架构 2.8 同源**：2.8 说「复合体 = 基准 + 局部坐标」，这里的 `origin` 就是那个「基准」——
   两处是同一个概念，实现时共用同一份数据，不要各写一套。
4. **玩家 `CharacterVirtual` 同样遵循**：它的 `SetPosition`/`GetPosition` 也是 `RVec3`。
5. **重新定原点的阈值待定**（建议：结构离原点超过 ~10 km 时触发；当前世界只有几百米，1.0 可以先不做，
   但**数据结构里要留 origin 字段**，否则以后再补要动所有位置换算）。

### 步骤 1（已完成）：构建集成

Jolt 源码已在 `third_party/JoltPhysics`（**v5.6.0**，`e77f175`），CMake 之前完全没引用。现已接入：

```cmake
add_subdirectory("${THIRD_PARTY_DIR}/JoltPhysics/Build" EXCLUDE_FROM_ALL ${CMAKE_BINARY_DIR}/jolt)
target_link_libraries(sandbox_engine PRIVATE ... Jolt)
```

**三个必须关掉的默认选项**（都是踩出来的）：

| 选项 | Jolt 默认 | 不关的后果 |
|---|---|---|
| `TARGET_SAMPLES` / `TARGET_HELLO_WORLD` / `TARGET_VIEWER` / `TARGET_PERFORMANCE_TEST` / `TARGET_UNIT_TESTS` | **全 ON** | `Build/CMakeLists.txt` 里 `if()` 直接 include 子工程，会连带编译示例，并引入 TestFramework / Assets 依赖 |
| `OVERRIDE_CXX_FLAGS` | **ON** | 覆盖全局 `CMAKE_CXX_FLAGS_DEBUG/RELEASE`，连带影响三个模组 DLL target（`/utf-8`、告警、运行库） |
| `USE_STATIC_MSVC_RUNTIME_LIBRARY` | **ON** | 把 `CMAKE_MSVC_RUNTIME_LIBRARY` 设成 `/MTd`，与引擎的 `/MDd` 冲突 → `LNK2038 RuntimeLibrary 不匹配` |

另设 `DOUBLE_PRECISION=OFF`（float 内核 —— 配合"世界原点偏移"，见上节「精度方案」）、
`USE_ASSERTS=OFF`（Debug 断言会显著拖慢构建与运行，需要时单独开）。

结果：`Jolt.lib` 64.6 MB 静态库编出；exe 仍 4.19 MB（**目前无源码引用，符号未进 exe**）；冒烟 9/9 PASS，行为零变化。

### 步骤 2（下一步）：最小验证

在动引擎之前，先在 `SelfTest` 里加一条**临时**自检：
建 `PhysicsSystem` + 地面 + 一个箱子 → 跑 100 步 → 断言箱子下落并停在预期高度。
跑通说明编译 / 链接 / 初始化 / 精度 / 断言全部没问题；验证完可删或留作长期 smoke。

### 第三方目录清理

同时删掉了两个目录里的示例与 GitHub 相关内容（共 24.5 MB）：

| 目录 | 删除 |
|---|---|
| `third_party/JoltPhysics` | `.github` / `Assets` / `HelloWorld` / `JoltViewer` / `PerformanceTest` / `Samples` / `TestFramework` / `UnitTests`（保留 `Jolt/` 源码、`Build/` cmake、`Docs/`、`LICENSE`、`.git`（版本锚点）） |
| `third_party/spdlog` | `.github` / `bench` / `example` / `logos` / `scripts` / `tests`（保留 `include/` `src/` `cmake/` `CMakeLists.txt` `LICENSE`） |

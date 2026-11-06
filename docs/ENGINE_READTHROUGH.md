# 引擎通读记录

> **目的**：在 DLL 模组加载器（#41）完成、引擎雏形基本收口之际，逐层通读 `src/` 全部代码，
> 确认各层职责边界，并记录发现的问题与缺口。
>
> **纪律**：发现先记在这里，再按严重度逐条处理（2026-11-04 起进入"记录 → 解决"两段式）。
> 每条解决后在条目末尾追加"解决记录"，写明改法与验证方式，不删原文（保留判断依据）。
>
> **范围**：`src/` 下 78 个 `.cpp/.hpp`，共 6,575 行（不含 `modapi/`、`mods_src/`）。
>
> **读法**：按数据流主线分 7 组读，而不是按目录顺序。目录顺序会把"谁调谁"拆散，
> 数据流顺序能让每一层的输入输出自然接上上一层的输出。
>
> **记录格式**：`R<n>` = 发现的问题（Risk/Remark），`Q<n>` = 需要拍板的待确认项。
> 每条都带文件与行号，便于回查。

---

## 通读进度

| 组 | 内容 | 规模 | 状态 |
|---|---|---|---|
| 1 | 数据真源层（`registry/` + `business/` + `shared/`） | ~700 行 | **已通读**（R1–R7 全部关闭） |
| 2 | 世界生成（`worldgen/` 全部） | ~750 行 | 未开始 |
| 3 | 跨线程（`SPSCQueue` / `ThreadLoop` / `TimeManager`） | ~200 行 | 未开始 |
| 4 | 输入（`InputSampler` / `CameraState` / `PlayerInput`） | ~220 行 | 未开始 |
| 5 | 渲染（`VulkanContext` / `Renderer` / `VulkanMesh` / `assets/` / `utils/` / `hud/` / `optimization/`） | ~2,950 行 | 未开始 |
| 6 | 模组宿主（`modhost/` + `modapi/`） | ~230 行 | 未开始 |
| 7 | 入口与装配（`main.cpp`） | 265 行 | 未开始（此前已多次读过，最后复核） |

---

## 第 1 组：数据真源层

### 已确认的结构（截至本轮）

| 组件 | 职责 | 关键点 |
|---|---|---|
| `business::World` | **全世界唯一的数据真源**：方块存储 + 锁 + 查询 + 动作入口 | 只有它能持有方块数据；所有修改必须过它的锁（"锁跟着数据走"） |
| `business::BusinessWorld` | 业务线程调度器（25 TPS） | 不含方块存储、锁、队列；只委托 `World` 裁决、委托两座桥排队与发桥 |
| `registry::Registry<T, Key>` | 泛型注册表（`unordered_map` + `frozen_`） | `frozen()` 之后 `add` 返回 false；读不加锁（前提：冻结后只读） |
| `registry::BlockInfo` | 方块属性纯数据 | `name` / `opaque` / `hardness` / `renderType` / `texture` / `model` |
| `shared::PhysicsCommand` / `PhysicsResult` | 业务 ↔ 物理的指令与回执 | `PhysicsCommand` 带 `std::vector<Block>`（全量同步 / 打包用） |
| `shared::BlockChangeEvent` | 业务 → 渲染的变更事件流 | 每 tick 一条，载荷是本 tick 变脏的 chunk 集合 |
| `business::base`（`BlockActions`） | 基础动作层：裁决 → 写入 → 标脏 | 无状态、无锁、纯函数；数据容器由调用方传引用；`markDirtyWithBoundary` 是"边界写要连带标邻居"的唯一实现 |
| `business::PhysicsBridge` | 业务 ↔ 物理的唯一通道（B1 指令 / B2 回执） | 两条 SPSC 队列；待发指令是单槽位，每 tick 最多发一条（优先级 同步 → 放置 → 破坏）；`push` 失败保留下 tick 重试 |
| `business::RenderBridge` | 业务 → 渲染的变更事件流（B3） | 事件语义"可延迟、不可丢"：`push` 成功才清脏集合，失败下 tick 重试 |

存储格式：`chunks_` 是 `unordered_map<ChunkPos, ChunkData>`，`ChunkData.blocks` 是 16³ = 4096 格的
密集 `uint8_t` 数组，只存方块类型（不存 `contraptionId`），空 chunk 不占内存。

锁策略：`World` 的**所有**读函数都加锁（`blockCount` / `renderSnapshot` / `chunkSnapshot` /
`chunkCount` / `forEachBlockInAABB` / `chunkDirtyFlags` / `findBlock`），写路径各自一次锁
（`placeBlock` / `removeBlock` / `applyGeneratedChunk` / `unloadChunks`）。
唯一不加锁的是 `dirtyChunksThisTick_`，因为只有业务线程碰它。

---

### 问题清单

#### R1 · `generateBlocks` 是逐方块查 map，不是逐 chunk 写

- **位置**：`src/business/World.cpp:192-203`
- **现象**：装配期批量生成时，每个方块都执行一次 `chunks_[chunkOf(b.pos)]`：

  ```cpp
  for (const auto& b : blocks) {
      auto& c = chunks_[engine::shared::chunkOf(b.pos)]; // 每块一次 hash 查找
      c.atWorld(b.pos) = static_cast<std::uint8_t>(b.type);
      c.markDirty(0x01);
  }
  ```

- **影响**：噪声基线 695,641 个方块 = 69 万次 `unordered_map` 查找，而实际 chunk 只有 220 个。
  超平坦基线 82,944 块也有 8 万次。这是启动阶段耗时的一部分（与首帧全量 mesh 重建并列）。
- **建议**：改成"先按 chunk 归并、再按索引直写"——外层遍历 chunk，内层遍历该 chunk 的方块，
  槽位只查一次。装配期单线程，不需要额外同步。
- **严重度**：~~中（一次性成本高，但不在每帧路径上）~~ → **降为"低 / 不修"**
- **复核结论（2026-11-04，改变定性）**：`generateBlocks` 的**唯一调用方**是
  `WorldGenerator::generate()`（`WorldGenerator.cpp:120`），而 `WorldGenerator` 在 1b 之后
  **已经没有任何调用方**——main 改用 `ColumnGenerator` + `ChunkStreamer`，运行期写入全部走
  `applyGeneratedChunk`（按 chunk 一次查找 + 4096 格直写，复杂度本来就是对的）。
  也就是说这条"逐方块查 map"的路径**实际从未被执行**，它属于死代码，不是热点。
  不做单点优化；等阶段 4 把生成器搬进 DLL 时，跨边界接口天然是 chunk 粒度，
  `generateBlocks` 会随 `WorldGenerator` 一起退役或删除。
- **✅ 已关闭（2026-11-04）：随 R6 一起删除**
  `generateBlocks` 的唯一调用方 `WorldGenerator` 已整文件删除，这条"逐方块查 map"的路径
  也一并从代码里消失，不存在"等阶段 4 再处理"的遗留。详见 R6 的解决记录。

#### R2 · `findBlock` 的注释与实际锁策略不符

- **位置**：`src/business/World.cpp:94-103`（注释在 95 行）
- **现象**：注释写"调用方需保证无并发写（业务线程持锁调用 / main 在三轨停止后调用）"，
  但函数内部**没有加锁**，而同文件的 `blockCount` / `renderSnapshot` / `chunkSnapshot` 都加了锁。
- **影响**：当前调用方确实都满足前提，所以不产生错误；但这条注释会让人以为"这个函数自带保护"，
  一旦有人在渲染线程或物理线程直接调用，就是无保护读 `unordered_map`（并发写时可能读到正在
  重建的桶，属未定义行为）。
- **建议**：二选一——① 补锁，与其他读函数统一；② 保留无锁快路径，但把注释改成明确的
  "**无锁调用**，调用方必须在业务线程内或三轨停止后调用，否则 UB"。倾向 ②（`findBlock`
  会被射线检测高频调用，加锁不划算）。
- **严重度**：中（当前不出错，属"未来踩坑"）
- **✅ 已解决（2026-11-04）：选 ①，补锁**
  - **复核推翻了"高频热点"假设**：`findBlock` 的调用点只有 2 处——`BusinessWorld::findBlock`
    的转发（`BusinessWorld.cpp:97`）和 DDA 射线（`BusinessWorld.cpp:175`）。
    射线最多推进 8 步（`for (int s = 0; s < 8; ++s)`），25 TPS × 8 ≈ **200 次/秒量级**，
    加锁成本可以忽略。原判断里的"高频"高估了一个数量级。
  - **改法**（`src/business/World.cpp:94`）：函数开头加
    `std::lock_guard<std::mutex> lock(blocksMutex_);`，把原注释改为"锁内访问 + 不可重入约束"，
    并写明调用频率依据。
  - **死锁排查**：逐一确认所有持锁函数都直接操作 `chunks_`，没有一个在锁内调用 `findBlock`
    （`placeBlock` / `removeBlock` / `applyGeneratedChunk` / `generateBlocks` /
    `forEachBlockInAABB` / `chunkDirtyFlags` 全部如此）；`base::tryPlace` / `tryRemove`
    接收的是容器引用，不接触 `World`，也不可能回调。
  - **验证（这次补到了真实路径）**：纯启动跑测的日志是 `放置/破坏=0/0`，说明被改的函数
    **一次都没执行**，仅编译通过不算数。改用 `EnumWindows` 按进程号找 GLFW 窗口 +
    `PostMessage(WM_LBUTTONDOWN 0x0201 / WM_LBUTTONUP 0x0202, lParam = y<<16 | x)` 注入 3 次点击后：
    计数从 `0/0` 走到 `2/7`，世界方块 82944 → 82940（破坏真实生效后又放置回来），
    tps 稳定 24.0–24.3，无卡死。DDA → `findBlock` → `removeBlock` 全链路跑通。

#### R3 · `chunks_` 只有增、没有删

- **位置**：`src/business/World.hpp:85`（`std::unordered_map<ChunkPos, ChunkData> chunks_;`）
- **现象**：1b 只实现了"请求 → 生成 → 应用"，没有卸载或驻留淘汰路径。`chunks_` 单调增长。
- **影响**：相机持续移动时内存无上限增长。目前 81 列规模下无感，但这是流式加载闭环的另一半。
- **建议**：与 `docs/DOCS自制版/` 里"流式加载与驻留架构决策"配套实现
  （按玩家距离淘汰 + 淘汰时通知渲染端删 mesh）。属既有决策的未落地部分，不是新问题。
- **严重度**：中（内存增长，长时间运行才显现）
- **✅ 已解决（2026-11-04）**
  - **设计三条硬约束**（后面任何改写都必须满足）：
    1. 淘汰只在业务线程做：`ChunkStreamer` 与 `applyGeneratedChunk` 同线程，是 `World` 的
       唯一写者，因此不需要额外锁窗口。
    2. 淘汰半径必须大于生成半径（当前 `radius_ + kEvictMargin = 4 + 1 = 5`）。
       窗口内层永不淘汰，这是"站在边界反复加载/卸载"不震荡的唯一保证。
    3. 渲染端不加新接口：`VulkanContext.cpp:742-750` 在"取不到任何可见面"时本来就
       `destroyChunkMesh` + `chunkMeshes_.erase`，卸载只要把被删 chunk 放进脏集合，
       网格会自己消失（不动 1,685 行的 `VulkanContext.cpp`）。
  - **改法（6 个文件）**：
    1. `World` 新增 `unloadChunks(const std::unordered_set<ChunkPos>&)`：一次加锁、
       命中项 `erase` 并计数；把**自身**放进 `dirtyChunksThisTick_`（让渲染端删旧网格）；
       把**仍存在的 6 邻居**也标脏（边界面从"被遮挡"变"暴露"）。六邻接偏移表提升为
       匿名 namespace 常量，与 `applyGeneratedChunk` 共用一份。
    2. `ChunkStreamer::tickUpdate` 前置淘汰阶段：遍历 `known_`，`|dx| > evictR || |dz| > evictR`
       的列进 `pendingEvict_` 并从 `known_` 擦除。
    3. `ChunkStreamer::drainResults` 先结算淘汰：每列展开 4 个竖直 chunk 组成 `dead` →
       `world.unloadChunks`；并**跳过 `known_` 里已不存在列的在途结果**（否则会把刚删的
       chunk 写回来）。返回 `DrainOutcome{ appliedColumns, evictedChunks }`。
    4. `main`：`drainResults` 之后，只要世界 chunk 集合有变化（**加载或卸载**）→
       `businessWorld.requestPhysicsResync()`。
    5. `BusinessWorld` 新增 `resyncPending_`：`PhysicsBridge::queueSync()` 在"已有待发指令"
       时返回 false，所以只置标志、每 tick 重试，成功才清；否则物理侧会一直留着
       已卸载区域的幽灵碰撞体。
    6. 生成侧无需改动：`known_` 在淘汰时已擦除，走回来会重新请求整列 4 层。
  - **验证（三步，含一次踩空与修正）**：
    1. **静止基线 12s**：`81/81/81`、方块 82,944、地表 `3..3`、世界chunk 81、注册表 7 个、
       `[Mod] 加载 1 个模组`、噪声 `frozen=true` 自检通过；stderr 仍是固定两行；
       mesh 83 行但唯一坐标 81 个（`(0,0,0)` 重建 3 次：面 516 → 512）；
       面数分布 `512×50 / 576×28 / 640×4` = 内部/边缘/角 黄金值。
    2. **按住 W 40s**（`EnumWindows` + `PostMessage`，W 是状态位）：chunk `81 → 90` 后
       稳定在 90 不再增长（`请求=108 应用=108`，即净淘汰 18 列）；业务 tps ≈24.8；
       截图 `build\r3_move_shot.png` 整片超平坦，**无穿透面、无竖直裂缝**。
    3. **物理一致性（第一次没通过）**：首版只在"有卸载"的 tick 重同步，
       复测得 `世界方块=92160` vs `物理碰撞体=82944`（少 9 列）。
       根因：卸载与分批加载错在不同 tick（每 tick 最多应用 4 列），
       **最后一次加载没有触发重同步**，物理停在中间态快照。
       改成"加载或卸载都重同步"后：`世界方块=99328` = `物理碰撞体=99328`
       （97 列 × 1024），物理线程日志 `碰撞体=99328` 也一致，`处理指令=14`。
  - **遗留**：淘汰半径取 `radius_+1` 是经验值（留一圈滞回）；若后续出现"走回原地重复生成"
    可再调。**模组动态卸载不在此列**——R3 只做世界/chunk 回收这一半。

#### R4 · chunk 索引解码在两处手写重复

- **位置**：`src/business/World.cpp:32-38`（`renderSnapshot`）、`81-87`（`chunkSnapshot`）
- **现象**：两处都手写同一套索引→世界坐标的换算：

  ```cpp
  const int bx = cp.x << 4, by = cp.y << 4, bz = cp.z << 4;
  out.push_back(Block{..., BlockPos{bx + (i & 15), by + ((i >> 4) & 15), bz + (i >> 8)}, -1});
  ```

  这与 `ChunkData` 内部的索引规则（x 取低 4 位、y 取次 4 位、z 取高 4 位）是同一套，
  但写成了两份独立实现。
- **影响**：改其中一处、忘了另一处，就会出现坐标错位——这类错误在体素引擎里非常难查
  （表现是"某些方块跑到别的 chunk 里"，而不是崩溃）。
- **建议**：在 `ChunkData` 上抽一个 `decodeIndex(i) -> ChunkLocalPos`（或反向的 `encodeIndex`），
  两处快照都调它。顺带把 `atWorld` 的索引计算也统一到同一处。
- **严重度**：低（当前正确），但属于"结构性隐患"
- **✅ 已解决（2026-11-04）**（行号为改动前的位置）
  - **改法（两处）**：
    1. `src/shared/block/Block.hpp` 的 `ChunkData` 里补一对严格互逆的静态函数：
       `indexOf(x,y,z)`（块内坐标 → 索引）与 `localOf(i)`（索引 → 块内偏移），
       并在注释里写明"改一处必须同步改另一处"。`atWorld` 的两个重载改为调用 `indexOf`，
       编码规则从三处收敛到一处。
    2. `src/business/World.cpp` 顶部匿名 namespace 增加 `appendChunkBlocks(cp, data, out)`，
       `renderSnapshot`（全量）与 `chunkSnapshot`（增量）的两段重复循环都改成调用它。
       两份快照的"遍历 + 跳过空气 + 拼世界坐标 + 塞 Block"逻辑从此只有一份。
  - **验证**：
    - 编译 `BUILD_EXIT=0`，零 warning。
    - **首帧 81 个 chunk 的 mesh 重建行与改动前基线逐字一致**（`Compare-Object` 结果：
      81 行全部匹配，零差异）。这一条是坐标解码正确性的硬判据——解码错位会立刻让面数/顶点数变化。
    - 第二次跑测多出 2 行 `面=515/513` 的重建记录，追下来是 **3.8–4.08s 之间发生了一次真实方块破坏**
      （世界方块 82944 → 82943、物理侧收到 1 条 `RemoveBlock` 指令），属正常运行路径。
      重跑一次（无任何交互）确认**未复现**：81 行 mesh 全 512 面、方块 82944、放置/破坏 0/0，
      说明那次是外部鼠标事件而非引擎或本次改动引起。
  - **更正（2026-11-04）**：后来逐行比对确认，"多出 2 行"的真实来源是 chunk 分批到达时
    `(0,0,0)` 被重建 3 次（首次 6 邻居未到齐，面 516；邻居到齐后修正为 512）。
    这与方块破坏无关——改动前的基线同样是 83 行日志 / 81 个唯一坐标。
    后续验收统一用"唯一 chunk 坐标数 = 81"作判据，不数日志行数。

#### R5 · `Registry::get` 在 Release 下没有保护

- **位置**：`src/registry/Registry.hpp:22-26`

  ```cpp
  const T& get(Key key) const {
      const T* p = find(key);
      assert(p && "get: Key未注册");
      return *p;               // Release 下 assert 被编译掉，p 为 nullptr 时解引用
  }
  ```

- **影响**：Debug 下是断言失败（能定位）；Release 下是空指针解引用，直接崩溃且没有信息。
  模组数量变多后，"查一个未注册 id"的概率会上升（比如预设 JSON 里写了不存在的方块名、
  或模组卸载后残留 id）。
- **建议**：`get` 改为返回 `const T*`（与 `find` 一致），或者保留引用返回但加一条 Release 也
  生效的兜底（返回一个静态 fallback `BlockInfo`）。考虑到调用方多数已经在用 `find`，
  统一到 `find` 是更小的改动。
- **严重度**：低—中（正确性风险，触发条件明确）
- **✅ 已解决（2026-11-04）**
  - **调查**：全工程 grep `Registry<>::get` 调用，**零调用方**——所有取方块信息的地方
    （`TextureAtlas.cpp:44`、`VulkanContext.cpp:679/709`、`ModLoader.cpp:62`）用的都是 `find` + 判空。
  - **改法**：直接删除 `get`（`src/registry/Registry.hpp`），并在原位置留注释说明为什么没有它；
    顺带删掉已无用的 `#include <cassert>`。
  - **理由**：与其把"Release 下会空指针解引用"的接口改成返回指针（那和 `find` 完全重复），
    不如不提供。调用方现有写法已经是最安全的形态。
  - **验证**：全量重编 `BUILD_EXIT=0`，零 error 零 warning。

#### R6 · `WorldGenerator` 整体是死代码（复核 R1 时发现）

- **位置**：`src/worldgen/generator/WorldGenerator.hpp`（32 行）/ `WorldGenerator.cpp`（112 行）
- **现象**：1b 之后 main 改用 `ColumnGenerator` + `ChunkStreamer`，`WorldGenerator::generate()`
  已无任何调用方。`ColumnGenerator.hpp:19-20` 写明保留理由："阶段 1 只新增、不替换，
  旧 generate() 保持原样以便基线回归"。
- **影响**：① 112 行代码 + 一份重复的地层解析逻辑（`findBlockIdByName` 在
  `WorldGenerator.cpp:48-52` 与 `ColumnGenerator.cpp:35-39` 各写了一遍）；
  ② 它让 `World::generateBlocks` 看起来是活路径，误导性能判断（见 R1 复核）。
- **建议**：阶段 4（生成器搬 DLL）时一并处理——要么删除，要么在文件头显式标注
  "仅保留用于基线回归，不参与运行路径"，避免后来人当成两套并行的生成器。
- **严重度**：低（不影响运行），但会持续制造误解
- **✅ 已解决（2026-11-04）：整条路径删除（选"删"而不是"标注保留"）**
  - **理由**：`ColumnGenerator` + `ChunkStreamer` 已完全替代它，1b 的基线回归也早已用新路径
    验证过（首帧 81 行 mesh 黄金值就是判据），留一份"永不执行"的生成器只会继续误导。
  - **改法**：
    1. 删除 `src/worldgen/generator/WorldGenerator.hpp`（32 行）与 `WorldGenerator.cpp`（112 行）。
    2. 删除 `World::generateBlocks` 的声明与实现；`World::World()` 注释改为
       "初始世界由装配层（main）用 `ColumnGenerator` + `ChunkStreamer` 从数据预设生成"。
    3. `World.hpp` 里 `applyGeneratedChunk` 的注释整段改写：不再用"与 `generateBlocks` 的区别"
       定义自己，改为直接说明职责（1b 流式加载 / 装配期首批地形都走这里），
       并保留四条约束（必须进 `dirtyChunksThisTick_`、邻居标脏、全空气不创建、业务线程调用）。
    4. `BusinessWorld` 构造注释同款改写。
    5. `CMakeLists.txt` 未改：`src/*.cpp` 走 `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` 自动收集。
  - **验证**：`BUILD_EXIT=0`，零 error 零 warning，两产物（`hello_block.dll` / `sandbox_engine.exe`）
    重链成功；静止基线数值与 R3 验证一逐项相同。

#### R7 · 点击事件拆成两个独立原子变量，业务侧跳号会吞点击

- **位置**：`src/input/InputSampler.cpp:122-133`（写）+ `src/business/BusinessWorld.cpp:126-131`（读）
- **现象**：鼠标回调先 `clickButton.store(0/1)`、再 `clickSeq.fetch_add(1)`，用两个独立原子变量
  拼成"一次点击"。业务侧用 `clickSeq` 去重，再单独 `clickButton.load()` 取按钮——
  两者之间没有 happen-before（先行发生）关系（都是 `relaxed`）。
  x86 的 store-store 不重排，实际能工作；但这是靠硬件兜底，不是代码保证的语义。
- **更实际的一处**：业务侧 `lastProcessedClickSeq_ = seq;` 直接把游标跳到最新序号。
  一个业务 tick（40ms）内若落进两次点击，**前一次被静默吞掉**，只执行后一次。
- **实测**：注入 3 次左键（3 组 down/up）后日志记到 `放置/破坏=2/7`（共 9 次动作），
  期间显然还有真实鼠标事件进来；但无论多少条，全部被逐条消费，无死锁、无数据错乱。
- **影响**：快速连点/双击时表现为"点了没反应"。不损坏数据，只丢动作。
- **建议**：把 (seq, button) 合成一个 64 位原子（低 32 位序号、高 32 位按钮），业务侧一次
  `load` 拿到完整事件；想不丢就改成"逐条处理直到追平 seq"，接受丢失就把现有行为写进注释
  （现在是隐式的）。
- **严重度**：低（交互体验，不影响数据真源正确性）
- **✅ 已解决（2026-11-04）：改用 SPSC 队列，不用"合成 64 位原子"**
  - **为什么换方案**：合成原子只能解决"两个原子之间没有 happen-before"，
    解决不了"按序号跳号会吞事件"。队列天然既不丢、也不需要序号，
    两个毛病一次解决。
  - **改法**：
    1. `PlayerInput` 整体重写：`struct ClickEvent { int button; }`（0 = 左键破坏，
       1 = 右键放置）+ `core::SPSCQueue<ClickEvent> clicks{32}`；保留
       `std::atomic<bool> toggleHud`。生产者是 GLFW 鼠标回调（单线程），符合 SPSC。
       头注释写明三条理由（无先行发生保证 / 跳号吞事件 / 队列天然不丢）。
    2. `InputSampler::mouseButtonCb`：`action != GLFW_PRESS` 直接 return，之后
       `clicks.push(ClickEvent{0})` / `{1}`，删掉 `clickButton.store` 与 `clickSeq.fetch_add`。
    3. `BusinessWorld::processPlayerInput` 重写为 `while (clicks.clicks.pop(ev))`：
       相机槽在 tick 内只读一次（`cx/cy/cz/yaw/pitch`、`cosP`、`dx/dy/dz`、`ox/oy/oz`、
       `tDelta*`、`step*` 全部出循环）；DDA 状态（`tMax*`、`ix/iy/iz`、`dir*`、`hit`、
       `hx/hy/hz`、放置位 `px/py/pz`）每次点击在循环内重初始化；
       `playerActions_.fetch_add(1)` 每处理一条 +1；`if (!hit) continue;`（原为 `return`，
       会中断同一 tick 的后续事件）；按 `ev.button` 分派破坏 / 放置。
    4. 删除成员 `lastProcessedClickSeq_`。
  - **验证（运行时，不只是编译通过）**：`EnumWindows` 取 GLFW 句柄 +
    `PostMessage(0x0201/0x0202)` 注入 2 次左键（间隔 5ms，远小于 40ms 一个 tick）+ 1 次右键：
    - `放置/破坏 = 1/2(拒0/0)`，世界方块 `82944 → 82942（破坏 2）→ 82943（放置 1）`；
    - 物理 `处理指令=3`、`碰撞体=1`；
    - stderr 仍固定两行；`WM_CLOSE` 优雅退出，日志出现"引擎退出。"。
    - **判据**：旧跳号方案下同一 tick 的第二次点击会被吞，破坏只会 +1；实测 +2，
      证明两条事件都被消费。
  - **附带结论（验收方法）**：强杀进程会丢 stdout 末段缓冲，日志看起来"停在被杀瞬间"，
    极易误判成线程卡死。验收一律用 `WM_CLOSE` 走优雅退出。

---

### 与本阶段任务（DLL 模组主线）的关联

- **R5 直接关系到阶段 5（卸载完整）**：`Registry` 目前没有 `erase`，卸载后的 id 也不会回收。
  如果要做"卸载模组 → id 释放 → 重新加载"，`Registry` 需要补 `erase` + 空闲 id 分配，
  否则反复重载会耗尽 `uint8_t` 的 255 个槽位。这一点在写阶段 5 之前必须先定。
- ~~**R1 与阶段 4（生成器搬进 DLL）相关**~~：R1 已随 R6 关闭——`generateBlocks` 删除后，
  阶段 4 的跨边界接口天然是 chunk 粒度，不存在"逐方块查 map 被放大到运行期"的问题。
- **R3 与阶段 5（卸载完整）相邻但不同**：R3 只做世界/chunk 回收；模组 DLL 的动态卸载
  仍搁置，且要先补 `Registry::erase` + 空闲 id 分配。

---

## 待确认事项

### Q1 · HUD 字体图集是否入库

- **现状**：`assets/textures/font/ascii.png`（160×96，2,286 字节）**未被 git 跟踪**。
  `.gitignore` 第 19 行 `assets/textures/` 忽略了整个目录（原意是挡 MC 版权纹理与网络素材）。
- **影响**：clone 后直接跑引擎，HUD 初始化加载不到图集，会失败。
- **选项**：

  | 选项 | 做法 | 说明 |
  |---|---|---|
  | A | `.gitignore` 从 `assets/textures/` 收窄为 `assets/textures/block/`，放行 `font/` | 该 PNG 由 `tools/gen_font_atlas.ps1` 用本机 Consolas 自绘，不涉及第三方版权 |
  | B | 保持现状 | clone 后先跑 `tools/gen_font_atlas.ps1`（脚本已入库） |

- **定案（2026-11-04）：选 B，保持现状。**
  理由：该图集只是 HUD 调试浮层的辅助资产，缺它不影响引擎主链路；生成脚本
  `tools/gen_font_atlas.ps1` 已入库，clone 后可重新生成。`.gitignore` 的既有语义
  （纹理资产不入库）保持不变，不为一张自绘 PNG 开例外。
- **✅ 待办已完成（2026-11-04）**：README 的"构建"节已补"首次运行前先跑
  `tools\gen_font_atlas.ps1`"（写明 160×96、依赖本机 Consolas、只影响 HUD 浮层）。
  改写后复查文件头仍是 `EF BB BF`，BOM 未丢。

---

## 后续追加约定

- 每读完一组，在"通读进度"表里更新状态，并在对应小节追加问题。
- 问题编号全局递增，不按组重排；已在后续组被推翻的条目，标注 `~~作废~~` 并写明原因，
  不删除（保留判断过程）。
- 全部读完后再统一出"缺口清单 + 优先级"，不在中途下总论。

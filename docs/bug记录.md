# bug 记录

> 只记录不修。严重度：🔴 会崩 / 🟡 行为错（条件触发）/ 🟢 隐患或诊断缺失
> 来源：`通读记录/U12` + `通读记录/文件分析/**`（18 份文档、约 176 处标记）。
> **核实**列：对着当前代码逐条验过（拆分/改名可能让文档过时）。✅属实 / ❌不实 / 🔧已修复 / ⏳待核实
> 性能问题见 `性能问题记录.md`（本文只收行为/正确性缺陷）。

## 一、模组宿主（modhost）　✅ 全部已核实

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B1 | 生命周期顺序 | `ModLoader.cpp` `unloadAll` | `destroy` 早于 `eraseByOwner` | 窗口期内注册表留指向已销毁对象的指针（`animUser` 悬垂） | 🟡 | ✅ |
| B2 | 逻辑漏判 | `ModSorter.cpp` `sort` | 跨批依赖"环上模组"不被拒（级联早于环检测） | 进 order 却缺依赖，`failedIds` 兜不住 | 🟡 | ✅ |
| B3 | 缺校验 | `BlockApi.cpp` `registerBlockImpl` | `mass/hardness/friction` 无 isfinite/范围校验 | 接 Jolt 后污染质量与摩擦 | 🟡 | ✅ |
| B4 | 缺校验 | `BlockApi.cpp` `registerBlockImpl` | 同名方块不检测 | `findBlockIdByName` 结果依赖哈希桶 | 🟢 | ✅ |
| B5 | 隐式依赖 | `ModLoader.cpp` `loadAll` | builtin 组内按 id 字典序 | 新增 `aaa_*` 会让方块 id 整体位移（存档错位） | 🟡 | ✅ |
| B6 | 诊断缺失 | `LogImpl.cpp` `logImpl` | 模组日志 tag 固定 `"Mod"` | 多模组日志无法归属 | 🟢 | ✅ |
| B7 | 回滚不完整 | `ModLoader.cpp` `loadAll` 失败分支 | 只回滚噪声，实体类型残留 | `onAnimate` 指向已卸载 DLL | 🟡 | ✅ |
| B8 | 异常无防护 | `ModLoader.cpp` / `onLoad` | C 边界无 try/catch | C++ 模组抛异常即 UB | 🟢 | ✅ |

## 二、基础库 core（ThreadLoop 12 条 / TimeManager 6 条）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B9 | 重复启动 | `ThreadLoop.hpp:25` `start` | 未拦 joinable，直接 `thread_ = std::thread(...)` | **terminate** | 🔴 | ✅ |
| B10 | 空回调 | `ThreadLoop.hpp:27` | `body` 为空仍启动 → `body()` 抛 `bad_function_call` | **terminate** | 🔴 | ✅ |
| B11 | noexcept 误用 | `ThreadLoop.hpp:31-34` `stop` | `noexcept` 内 `join()`（可抛） | self-join/失败 → **terminate** | 🔴 | ✅ |
| B12 | 异常未捕获 | `ThreadLoop.hpp:25-28` | 线程入口无 try/catch | body 抛异常 → **terminate** | 🔴 | ✅ |
| B13 | 状态残留 | `ThreadLoop.hpp:22` | 先置 `running_=true` 再构造 `thread_`；构造抛则状态不一致 | 之后 `stop`/`shouldStop` 行为异常 | 🟡 | ✅ |
| B14 | 并发无约定 | `ThreadLoop.hpp` | `start/stop` 未注明非线程安全 | 并发调用 UB | 🟡 | ✅ |
| B15 | self-join | `ThreadLoop.hpp` | body 内调 `stop()` | 见 B11 | 🟡 | ✅ |
| B16 | 缺 include | `ThreadLoop.hpp` | 用 `std::move` 但未含 `<utility>` | 依赖传递包含（可移植性） | 🟢 | ✅ |
| B17 | 移动未禁 | `ThreadLoop.hpp` | 只 delete 了拷贝，移动未显式 delete（因 atomic 隐式删） | 意图不清晰 | 🟢 | ✅ |
| B18 | 命名反直觉 | `ThreadLoop.hpp:36` `shouldStop` | 语义是 `!running_`，字段叫 `running_` | 易写反 | 🟢 | ✅ |
| B19 | 内存序 | `ThreadLoop.hpp` | 全 `relaxed`（含停止标志） | 停止延迟（高频循环可接受） | 🟢 | ✅ |
| B20 | 静默归域 | `TimeManager.hpp:78-83` `domainOf` | 非 Physics 一律归 business（含非法值） | 改错轨速度且无提示 | 🟡 | ✅ |
| B21 | 无输入校验 | `TimeManager.hpp:25` `setGlobalScale` | 不校验 NaN/负数/0 | 下游 dt 异常 | 🟢 | ✅ |
| B22 | 未显式禁拷贝 | `TimeManager.hpp` | 靠 atomic 成员隐式不可拷贝 | 意图不明 | 🟢 | ✅ |
| B23 | 缓存行 | `TimeManager.hpp:85-87` | 三个原子同缓存行 | 写频率极低，可忽略 | 🟢 | ✅ |
| — | ~~调试用无隔离~~ | `TimeManager.hpp:33-54` | — | — | — | ❌ **不实**：`setDomainScale` 上方有 20 行长注释明令禁止运行期使用 |
| — | ~~effectiveScale 注释叠在一起~~ | `TimeManager.hpp:64-75` | — | — | — | ❌ **不实**：注释分段清晰 |

## 三、共享数据结构 shared（Block 9 / ChunkPos 7 / ChunkData 6 / PlayerInput 6 / PlayerBodyState 5 / CameraState 8）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B24 | 哈希质量 | `Block.hpp:53-59` `hash<BlockPos>` | 乘 31 混合，可构造碰撞（如 `(1,0,0)` 与 `(0,31,0)` 同为 31） | 哈希表退化 | 🔴 | ✅ |
| B25 | 哈希质量 | `ChunkPos.hpp:37-43` `hash<ChunkPos>` | 与 BlockPos 同款弱混合 | 同上 | 🔴 | ✅ |
| B26 | 冗余分号 | `Block.hpp:59`、`ChunkPos.hpp:43` | 函数体后多一个 `;` | 无害 | 🟢 | ✅ |
| B27 | 语义双关 | `Block.hpp:43` `contraptionId` | `-1` 表主世界，无 `isMainWorld()` 辅助 | 调用点难读（源码里已自带"这里有些问题"注释） | 🟡 | ✅ |
| B28 | 可简化 | `Block.hpp:30`、`ChunkPos.hpp:20` `operator==` | 可 `= default`（C++20） | 无 | 🟢 | ✅ |
| B29 | 类型宽度 | `Block.hpp:53`、`ChunkPos.hpp:37` | 用裸 `size_t` | 风格不统一 | 🟢 | ✅ |
| B30 | 依赖过重 | `ChunkPos.hpp:2` | 为 `BlockPos` include 整个 `Block.hpp` | 编译耦合（应拆 `BlockPos.hpp`） | 🟡 | ✅ |
| B31 | 重复定义 | `ChunkPos.hpp` / `Block.hpp` | 两个坐标类型结构几乎一致 | 可用模板统一（原型可不做） | 🟢 | ✅ |
| — | ~~`>> 4` 耦合未文档化~~ | `ChunkPos.hpp:26` | — | — | — | ❌ **不实**：已有注释"这里 >> 4 强绑定区块大小" |
| B32 | 可简化 | `ChunkData.hpp:174,183,190` | `std::vector<T>().swap(v)` 释放 hack | 可读性 | 🟢 | ✅ |
| B33 | 提前分配 | `ChunkData.hpp:199-215` `chunkCompact` | "一项都没丢"的提前返回发生在 `used`/`remap`/`nextPal` 分配之后 | 每次调用白分配三个容器 | 🟢 | ✅ |
| B34 | 越界不设防 | `ChunkData.hpp:60` `chunkLocalOf` | 对 `i` 越界不检查 | 内部约定（可加 assert） | 🟢 | ✅ |
| B35 | 缺拷贝声明 | `ChunkData.hpp` | 未显式 delete 拷贝（默认行为正确） | 意图不明 | 🟢 | ✅ |
| B36 | 线性查找 | `ChunkData.hpp:114-123` `paletteFindOrAdd` | 线性扫描字典 | 小调色板下是设计选择 | 🟡 | ✅（设计取舍） |
| — | ~~`dirty` 是 public 可绕过 `markDirty`~~ | `ChunkData.hpp:52` | — | — | — | ❌ **不实**：`dirty_` 私有，`dirty()` 是只读 getter，只能经 `markDirty` 改 |
| B37 | 注释与实现不符 | `PlayerInput.hpp:12` | 注释"满时返回 false，可下 tick 重试"，但鼠标回调只 push 一次、不看返回值 | 读者以为有重试机制 | 🟡 | ✅ |
| B38 | 语义未说明 | `PlayerInput.hpp:28,31` | `toggle*` 连按两次会被合并成一次（`exchange(false)` 只消费一次） | 快速连按丢一次切换 | 🟡 | ✅ |
| B39 | 信息缺失 | `PlayerInput.hpp:13-15` `ClickEvent` | 只有 `button`，没有点击位置 | 隐式假设"准星在屏幕中心"，未写明 | 🟡 | ✅ |
| B40 | 容量语义 | `PlayerInput.hpp:20` | `clicks{32}` 是 32 个事件（实现内部多一个哨兵槽） | 易误算 | 🟢 | ✅ |
| B41 | 缺拷贝声明 | `PlayerInput.hpp` | 未显式 delete 拷贝 | 意图不明 | 🟢 | ✅ |
| B42 | 伪共享 | `PlayerInput.hpp:24-31` | 三个 `toggle*` 同缓存行 | 写频率低，不改 | 🟢 | ✅ |
| B43 | 内存序缺口 | `PlayerBodyState.hpp:56-67` `readInterpolated` | 数据用 `relaxed` 读，第二次 `seq` 用 acquire，但缺**读后 acquire fence** | **ARM64 上 relaxed 读可被重排到 `s1` 之后 → 采信半新半旧数据** | 🔴 | ✅ |
| B44 | 自防御缺失 | `PlayerBodyState.hpp:48` `readInterpolated` | 函数自身不检查 `valid`（靠调用方先查） | 首帧若漏查则读到全 0 位置 | 🟡 | ✅ |
| B45 | 类型宽度 | `PlayerBodyState.hpp:23,34,35` | `unsigned long long` / `long long` | 跨平台宽度不确定 | 🟡 | ✅ |
| B46 | lock-free 未断言 | `PlayerBodyState.hpp:23` | `atomic<uint64_t>` 未加 `static_assert(is_always_lock_free)` | 非 lock-free 平台上"原子"变锁 | 🟢 | ✅ |
| — | ~~写侧 seqlock 规则没说明~~ | `PlayerBodyState.hpp:20` | — | — | — | ❌ **不实**：第 20 行已写明"奇数表示写者正在写，偶数且读前后一致才采信" |
| B47 | 原子前提 | `CameraState.hpp:12-16` | `atomic<double>` 未断言 lock-free（`TimeManager` 有，这里没有） | 非 lock-free 平台无保证 | 🔴 | ✅ |
| B48 | 非一致快照 | `CameraState.hpp` | 5 个字段各自读，无 seqlock | 可能读到"新位置 + 旧朝向"（一帧错位） | 🟡 | ✅ |
| B49 | 裸 int 枚举 | `CameraState.hpp:21` `viewMode` | `atomic<int>` 而非 `enum class ViewMode` | 非法值无约束 | 🟡 | ✅ |
| B50 | 内存序 | `CameraState.hpp` | 全默认 `seq_cst` | 可放宽（可选优化） | 🟢 | ✅ |
| B51 | 伪共享 | `CameraState.hpp` | 5-6 个原子同缓存行 | 写频率低，不改 | 🟢 | ✅ |
| — | ~~Vulkan: double→float / UBO 双缓冲 / 投影约定~~ | `CameraState` → 渲染端 | — | — | — | ⏳ **不属于本表**：渲染端使用注意项（规则 2/5 豁免渲染），归 U16 文档 |

## 四、协议 shared/protocol（12 条）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B52 | 默认值错 | `Protocol.hpp:52` `PhysicsResult::contraptionId` | 默认 `0`（`PhysicsCommand` 同字段是 `-1`） | 未赋值回执被当成"刚体 0 的消息" | 🔴 | ✅ |
| B53 | 命名不统一 | `Protocol.hpp:67` | `std::vector<engine::shared::Block>` 与同文件 `std::vector<Block>` 混用 | 可读性 | 🟡 | ✅ |
| B54 | 适用范围不清 | `Protocol.hpp:47-48` | `processedCommands` / `simTime` 注释只说含义，没说"哪些 type 会带" | 同步校验逻辑难维护 | 🟡 | ✅ |
| B55 | 单文件多义 | `Protocol.hpp` | 一个文件塞 5 套类型（命令/回执/结构事件/位姿/变更事件） | 方向与所有权难分辨 | 🟡 | ✅ |
| B56 | 注释不全 | `Protocol.hpp:61` `nx/ny/nz` | 只写"碰撞法向"，未写取值 `{-1,0,1}` | 使用者易误以为是连续分量 | 🟢 | ✅（部分） |
| B57 | 信息缺失 | `Protocol.hpp:26` `dx/dy/dz` | MoveBlock 无起点坐标 | 位移指令不可重放 | 🟢 | ✅ |
| B58 | 结构偏胖 | `Protocol.hpp:40-68` | `PhysicsResult` 20+ 字段承载 6 种消息 | 每条约 200B（队列 64 深） | 🟢 | ✅ |
| — | ~~BodyPose/RigidBodyPos 缺旋转~~ | — | — | — | — | 🔧 **已修复**：`qx..qw` 已加入（staged rotation） |
| — | ~~x/y/z 注释只提 MoveBlock~~ | — | — | — | — | ❌ **不实**：现在只用于 BlockMoved，注释"移动后坐标"正确 |
| — | ~~事件流/状态语义缺机制保证~~ | `Protocol.hpp:70-92` | — | — | — | ❌ **不实**：已写明"逐条送达、不能丢"与"中间帧可丢" |

## 五、注册表 registry（BlockInfo 6 / BlockRegistry 9 / Registry 6）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B59 | 语义重叠 | `BlockInfo.hpp` `opaque` vs `renderType` | 两字段都可表达"是否遮挡剔除" | 矛盾时行为不可预期 | 🔴 | ✅ |
| B60 | 注释缺失 | `BlockInfo.hpp` `hardness` | 无单位/无含义 | 模组值写错无人发现 | 🟡 | ✅ |
| B61 | 描述绕 | `BlockInfo.hpp` `texture`/`model` | 关系与"两者都空"的边界没写清 | 模组作者困惑 | 🟡 | ✅ |
| B62 | 缺比较 | `BlockInfo.hpp` | 无 `operator==` | 需要时再加 | 🟢 | ✅ |
| B63 | 缺顺序说明 | `BlockInfo.hpp` `RenderType` | 未写渲染顺序 Solid→Cutout→Translucent | 分桶优先级不明 | 🟢 | ✅ |
| B64 | 定位不清 | `BlockInfo.hpp` | 未说明"它是注册表值类型" | 读者以为是通用方块定义 | 🟢 | ✅ |
| B65 | 跨 DLL 实例 | `BlockRegistry.hpp` `inline g_blockRegistry` | `inline` 变量在每 DLL 各有一份 | 模组直接访问会拿到空表（必须走回调） | 🔴 | ✅ |
| B66 | 复杂度 | `BlockRegistry.hpp` `findBlockIdByName` | O(N) 线性遍历 | 方块多时加载变慢 | 🟡 | ✅ |
| B67 | 魔法数 | `BlockRegistry.hpp` | `0xFFFF` 出现 4 次 | 改一处漏一处 | 🟡 | ✅ |
| B68 | 缺校验 | `BlockRegistry.hpp` `resolveBuiltinBlockIds` | 未检查 `frozen()` | freeze 前调用会解析到空表 | 🟡 | ✅ |
| B69 | 警告语义 | `BlockRegistry.hpp` | 只 warn stone/dirt/grass，未说明为何只这三个 | 读者以为漏检 | 🟡 | ✅ |
| B70 | 缺断言 | `BlockRegistry.hpp` | 无 `static_assert` 保证 key 能表示 `0xFFFF` 哨兵 | 改 key 类型即静默错 | 🟢 | ✅ |
| B71 | 静默失败 | `BlockRegistry.hpp` `freezeBlockRegistry` | 提前调用只静默少方块 | 无线索可查 | 🟢 | ✅ |
| B72 | 重名不检测 | `BlockRegistry.hpp` | 不检查方块名唯一（与 B4 同源） | `findBlockIdByName` 结果不定 | 🟢 | ✅ |
| B73 | 约束未写 | `BlockRegistry.hpp` `g_builtinBlockIds` | 未写"启动时 resolve 一次，之后只读" | 运行期改写即竞争 | 🟢 | ✅ |
| B74 | 线程契约缺失 | `Registry.hpp` `erase`/`eraseByOwner` | 非线程安全，注释未写明 | 其它线程清表即数据竞争 | 🔴 | ✅ |
| B75 | 异常安全 | `Registry.hpp` `add` | `owners_.emplace` 抛异常时 `items_` 已插入 | 两表不一致（owner 丢失） | 🟡 | ✅ |
| B76 | 能力缺失 | `Registry.hpp` | `const_iterator` 拿不到 owner | 只能另调 `ownerOf(key)` | 🟢 | ✅ |
| B77 | 断言缺失 | `Registry.hpp` `erase` | 忽略 `owners_.erase` 返回值 | 两表可能长度不一致 | 🟢 | ✅ |
| B78 | 缺 clear | `Registry.hpp` | 没有 `clear()` | 需要时再加 | 🟢 | ✅ |

## 六、业务层 business（World 11 / BusinessWorld 13 / PhysicsBridge 5 / RenderBridge 7 / BlockActions 9）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B79 | 注释矛盾 | `World.hpp:18` 构造函数 | 注释"48×48 石头平台"，实现已不预置（改由模组+流式生成） | 按注释推理必错 | 🔴 | ✅ |
| B80 | 未显式禁拷贝 | `World.hpp` | 靠 mutex/map 隐式不可拷贝 | 意图不明 | 🟡 | ✅ |
| B81 | 线程模型分散 | `World.hpp` / `World.cpp` | 两处各写一半 | 易漏读 | 🟡 | ✅ |
| B82 | 功能重叠 | `World.hpp` `findBlock`/`lookupBlock` | 二态 vs 三态，关系未写清 | 调用方选错（见 B101） | 🟡 | ✅ |
| B83 | 容量偏小 | `World.cpp` `renderSnapshot`/`chunkSnapshot` | `reserve(×64 / ×512)`，实测约 3156 非空气/列 | 多次扩容（性能+峰值） | 🟡 | ✅ |
| B84 | 锁不对称 | `World.hpp` `dirtyChunksThisTick_` | 写在锁内、读（peek/clear）不加锁 | 靠约定维持，易误读为受锁保护 | 🟡 | ✅ |
| B85 | 无存盘前置检查 | `World.hpp` `unloadChunks` | 脏 chunk 直接删 | 接存档后会丢数据 | 🟡 | ✅ |
| B86 | 回调在锁内 | `World.hpp:62` `forEachBlockInAABB` | 回调在锁内执行、不可重入 | 回调里调 `getBlock` 会自死锁 | 🟢 | ✅（已注明） |
| B87 | 间接调用 | `World.hpp` `std::function` | 每个方块一次间接调用 | 原型可接受 | 🟢 | ✅ |
| B88 | 位移实现 | `World.cpp` `appendChunkBlocks` | `cp.x << 4`（C++20 定义良好） | 可读性（或改 `* 16`） | 🟢 | ✅ |
| B89 | 多余构造 | `World.cpp` `applyGeneratedChunk` | `chunks_[cp]` 默认构造再赋值 | 低频，无害 | 🟢 | ✅ |
| B90 | 封装泄漏 | `BusinessWorld.hpp` `world()` | 返回非 const `World&` 与注释矛盾 | 可绕过统计与排队 | 🟡 | ✅ |
| B91 | 约束未写 | `BusinessWorld.hpp` `placeBlock`/`removeBlock` | 内部写 SPSC 队列（单生产者），未注明"仅业务线程" | 其它线程调用破坏约定 | 🟡 | ✅ |
| B92 | 语义易混 | `BusinessWorld.hpp` `syncAll` vs `requestPhysicsResync` | 一次性 vs 置标志自动重试 | 调用方选错 | 🟡 | ✅ |
| B93 | 能力缺失 | `BusinessWorld.hpp` | 只转发 `findBlock`，未转发 `lookupBlock` | 想区分"未加载/空气"只能绕过封装 | 🟡 | ✅ |
| B94 | 覆盖丢状态 | `BusinessWorld.cpp` `receiveResultsFromPhysics` | 位姿单 `optional<BodyPose>` | 多刚体后到覆盖先到（玩家体与演示刚体一直互相覆盖） | 🟡 | ✅ |
| B95 | 返回值忽略 | `BusinessWorld.cpp` `pushBodyStructure` | 结构事件"不能丢"但返回值没人看 | 队列满静默丢 → 渲染缺网格 | 🟡 | ✅ |
| B96 | 每 tick 构造 | `BusinessWorld.cpp` | `bodyEvents` 局部 vector 每 tick 构造 | 可复用成员（收益有限） | 🟡 | ✅ |
| B97 | 语义缺失 | `BusinessWorld` DDA | `findBlock` 把"未加载"当"空气" | 射线穿过未加载区域（结果看起来随机） | 🔴 | ✅ |
| B98 | 已知缺口 | `BusinessWorld` 点击处理 | 右键永远放 Stone | 无法放置其它方块 | 🔴 | ✅ |
| B99 | 返回值忽略 | `BusinessWorld` 点击处理 | `placeBlock`/`removeBlock` 失败无反馈 | 玩家操作失败无感知 | 🟡 | ✅ |
| B100 | 脆弱约定 | `BusinessWorld` / `InputSampler` | 方向向量需与渲染端逐位一致（各写一份） | 任一侧改公式即错位 | 🟡 | ✅ |
| B101 | 判定遗漏 | `BusinessWorld` DDA | 起点格不参与判定（先推进再检查） | 站在方块里打不到它 | 🟡 | ✅ |
| B102 | 非原子标志 | `BusinessWorld.hpp` | `resyncPending_` / `firstTick_` 靠"业务线程独占" | 跨线程调 `requestPhysicsResync` 即竞争 | 🟢 | ✅ |
| B103 | 覆盖丢事件 | `PhysicsBridge.hpp:63-68` | `wantPlace_`/`wantRemove_` 是**单槽位** | 同 tick 第二条覆盖第一条 → 世界与物理不同步（看得见、穿得过） | 🔴 | ✅ |
| B104 | 死生产 | `PhysicsBridge.cpp` | 碰撞事件仍生产，无消费方 | 每次碰撞白进队列 | 🟡 | ✅ |
| B105 | 封装泄漏 | `PhysicsBridge.hpp:46-47` | 队列访问器公开可变引用 | 可绕过桥的统计与待发状态机 | 🟢 | ✅ |
| B106 | 死代码 | `BusinessWorld.hpp` `syncAll` | 从未被调用（grep 确认仅声明+定义） | 与 B92 叠加更难判断 | 🟢 | ✅ |
| — | ~~"queue" 名字骗人~~ | `PhysicsBridge` | — | — | — | 🔧 **已修复**：批次 6 改名 `push*Command` |
| B107 | 拷贝两次 | `RenderBridge.cpp` `flushChangesToRender` | `peekDirtyChunks()` 拷贝 → `assign` 再拷贝 | 每 tick 双倍 | 🟡 | ✅ |
| B108 | 队列满恶化 | `RenderBridge.cpp` `flushChangesToRender` | 失败不清脏 → 下 tick 拷更大一份 | 渲染落后时雪崩 | 🟡 | ✅ |
| B109 | 事件粒度过大 | `RenderBridge.cpp` `flushChangesToRender` | 一 tick 全部脏 chunk 塞一条事件 | 脏 100 个时渲染端一次重建 100 个 | 🟡 | ✅ |
| B110 | 背压不可见 | `RenderBridge` 整体 | 业务不知渲染消费到哪 | 积压只能靠症状发现 | 🟡 | ✅ |
| B111 | 容量不足 | `RenderBridge.hpp:44` | 结构事件"不能丢"但队列仅 16 深 | 同 tick 打包 20 个刚体即丢（B95 成因） | 🟡 | ✅ |
| B112 | 封装泄漏 | `RenderBridge.hpp:33-35` | 三条队列访问器公开 | 同 B105 | 🟢 | ✅ |
| B113 | 死字段 | `RenderBridge.cpp` | `BlockChangeEvent::tick` 只写不读 | 读者以为它用于去重/排序 | 🟢 | ✅ |
| B114 | 常量空缺 | `BlockActions.hpp:15-17` | DIRTY 只定义 0x01/0x08/0x10，缺 0x02/0x04 无说明 | 读者以为漏定义 | 🟡 | ✅ |
| B115 | 措辞错误 | `BlockActions.hpp:11` | 写"无状态、无锁、纯函数"，但 `tryPlace/tryRemove` 有副作用 | 误以为可无锁并发调用 | 🟡 | ✅ |
| B116 | 标脏不全 | `BlockActions.cpp` `markDirtyWithBoundary` | 只标 6 个面邻居，不标对角 | 接 AO/平滑光照时边缘漏更新 | 🟡 | ✅ |
| B117 | 缺前置检查 | `BlockActions.cpp` `tryPlace` | 不拒绝 `type == Air` | 标脏但数据没变，且**凭空建空 chunk** | 🟡 | ✅ |
| B118 | 异常留残留 | `BlockActions.cpp` `tryPlace` | `chunks[cp]` 先建 chunk，后续抛异常则留空 chunk | 多占内存 | 🟡 | ✅ |
| B119 | 归属不当 | `BlockActions.hpp` | 脏原因常量定义在此，而 `ChunkData` 才是使用者 | 概念错位 | 🟡 | ✅ |
| B120 | 命名不一致 | `BlockActions.cpp` | `tryPlace` 用 `cp`、`tryRemove` 用 `it` | 风格 | 🟢 | ✅ |
| B121 | 无检查 | `BlockActions.cpp` `markDirtyWithBoundary` | 邻居 chunk 不存在也插进 dirty | 渲染端需自行忽略 | 🟢 | ✅ |
| B122 | 缺 noexcept | `BlockActions.hpp` | 未标 `noexcept` | 正确（不强异常安全），可注释 | 🟢 | ✅ |

## 七、世界生成 worldgen（ChunkStreamer 12 条，与 U12 同源）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B123 | 失败不致命 | `main.cpp` `warmupWindow` 超时分支 | 预热 30s 超时只打印就继续启动 | 世界可能只灌一部分，渲染端可能一个 chunk 都没有 | 🔴 | ✅ |
| B124 | 负下标 | `ColumnGenerator.cpp` `generateColumn` | `out.chunks[y >> 4]`，`kGenMinY < 0` 时负下标 | **内存破坏** | 🔴 | ✅ |
| B125 | 闪洞 | `ChunkStreamer.cpp` `applyPendingEvictions` | 淘汰同 tick 结算，不复查是否又回到窗口 | 边界来回走时该列空 2~3 tick（80–120ms） | 🟡 | ✅ |
| B126 | 注释失真 | `ChunkStreamer.hpp:36` | 写"一次锁写 64KB"，实际一列 4 次锁 | 基于该注释的性能判断会错 | 🟡 | ✅ |
| B127 | 单 tick 尖峰 | `ChunkStreamer.cpp` `receiveResults` | 废弃结果 `continue` 不计 `applied`，队列积压时一次扫空 | 瞬移后长耗时掉帧 | 🟡 | ✅ |
| B128 | 隐式三环契约 | `World::unloadChunks` → `flushChangesToRender` → `rebuildChunkMeshes` | "无顶点则删 mesh"靠三环约定 | 任一环被"优化"即留幽灵网格 | 🟡 | ✅ |
| B129 | 线程归属无保护 | `ChunkStreamer.hpp` | 业务线程 + 生成线程共用一个对象，只靠注释 | 错误线程调用即数据竞争 | 🟡 | ✅ |
| B130 | 反向依赖 | `ChunkStreamer.cpp` `receiveResults(World&)` | worldgen → business::World（唯一反向依赖） | 生成器搬 DLL 走不通（规则 7 边界） | 🟡 | ✅ |
| B131 | 阻止移动 | `ChunkStreamer.cpp` `workerLoop` | `const GenResult res` 只能走 `push(const T&)` | shared_ptr 多一次原子加（可忽略） | 🟢 | ✅ |
| B132 | 空转 | `ChunkStreamer.cpp` `workerLoop` | 无请求时 `sleep(2ms)` | 稳定后每秒数百次唤醒 | 🟢 | ✅ |
| B133 | 反压未注明 | `ChunkStreamer.hpp:34` | `kMaxApplyPerTick=4` 与 `kResultCapacity=64` 的反压关系没写 | 调参时易忽略 | 🟢 | ✅ |
| B134 | 约束未注明 | `ChunkStreamer::stats()` | 聚合字段只能在业务线程读，无编译期防护 | 从 main 读即竞争 | 🟢 | ✅ |

---

## 汇总

| 项 | 数量 |
|---|---|
| 核实条目 | **134 条**（B1–B134） |
| 其中 🔴 | **13 条** |
| 文档误记（❌ 不实） | **8 条**（TimeManager ×2、ChunkData ×1、PlayerBodyState ×1、ChunkPos ×1、Protocol ×3） |
| 已被重构修复（🔧） | **3 条**（BodyPose 缺旋转、`queue*` 命名、"事件流语义"） |
| 归类为性能（→ `性能问题记录.md`） | 8 条（P1–P8） |
| 归类为渲染端注意项（规则 2/5 豁免） | 3 条（CameraState→Vulkan） |

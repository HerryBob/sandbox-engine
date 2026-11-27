# bug 记录

> 只记录不修。严重度：🔴 会崩 / 🟡 行为错（条件触发）/ 🟢 隐患或诊断缺失
> 来源：`通读记录/U12` + `通读记录/文件分析/**`（18 份文档、约 176 处标记）。
> **核实**列：对着当前代码逐条验过（拆分/改名可能让文档过时）。✅属实 / ❌不实 / 🔧已修复 / ⏳待核实
> 性能问题见 `性能问题记录.md`（本文只收行为/正确性缺陷）。

## 一、模组宿主（modhost）　✅ 全部已核实

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B1 | 生命周期顺序 | `ModLoader.cpp` `unloadAll` | `destroy` 早于 `eraseByOwner` | 窗口期内注册表留指向已销毁对象的指针（`animUser` 悬垂） | 🟡 | ✅已修（顺序改为 onUnload→清注册→destroy） |
| B2 | 逻辑漏判 | `ModSorter.cpp` `sort` | 跨批依赖"环上模组"不被拒（级联早于环检测） | 进 order 却缺依赖，`failedIds` 兜不住 | 🟡 | ✅已修 |
| B3 | 缺校验 | `BlockApi.cpp` `registerBlockImpl` | `mass/hardness/friction` 无 isfinite/范围校验 | 接 Jolt 后污染质量与摩擦 | 🟡 | ✅已修 |
| B4 | 缺校验 | `BlockApi.cpp` `registerBlockImpl` | 同名方块不检测 | `findBlockIdByName` 结果依赖哈希桶 | 🟢 | ✅已修 |
| B5 | 隐式依赖 | `ModLoader.cpp` `loadAll` | builtin 组内按 id 字典序 | 新增 `aaa_*` 会让方块 id 整体位移（存档错位） | 🟡 | ⏸ 已评估不改：同 phase 内按 id 字典序是隐式依赖，正确解是给 manifest 加 priority（接口扩展）。风险已写进 preSortCandidates 注释 |
| B6 | 诊断缺失 | `LogImpl.cpp` `logImpl` | 模组日志 tag 固定 `"Mod"` | 多模组日志无法归属 | 🟢 | ✅已修 |
| B7 | 回滚不完整 | `ModLoader.cpp` `loadAll` 失败分支 | 只回滚噪声，实体类型残留 | `onAnimate` 指向已卸载 DLL | 🟡 | ✅已修（回滚改用 cleanupOwnedContent） |
| B8 | 异常无防护 | `ModLoader.cpp` / `onLoad` | C 边界无 try/catch | C++ 模组抛异常即 UB | 🟢 | ✅已修 |

## 二、基础库 core（ThreadLoop 12 条 / TimeManager 6 条）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B9 | 重复启动 | `ThreadLoop.hpp:25` `start` | 未拦 joinable，直接 `thread_ = std::thread(...)` | **terminate** | 🔴 | ✅已修 |
| B10 | 空回调 | `ThreadLoop.hpp:27` | `body` 为空仍启动 → `body()` 抛 `bad_function_call` | **terminate** | 🔴 | ✅已修 |
| B11 | noexcept 误用 | `ThreadLoop.hpp:31-34` `stop` | `noexcept` 内 `join()`（可抛） | self-join/失败 → **terminate** | 🔴 | ✅已修 |
| B12 | 异常未捕获 | `ThreadLoop.hpp:25-28` | 线程入口无 try/catch | body 抛异常 → **terminate** | 🔴 | ✅已修 |
| B13 | 状态残留 | `ThreadLoop.hpp:22` | 先置 `running_=true` 再构造 `thread_`；构造抛则状态不一致 | 之后 `stop`/`shouldStop` 行为异常 | 🟡 | ✅已修（构造失败时回滚 running_） |
| B14 | 并发无约定 | `ThreadLoop.hpp` | `start/stop` 未注明非线程安全 | 并发调用 UB | 🟡 | ✅已修（注明只允许单线程调用） |
| B15 | self-join | `ThreadLoop.hpp` | body 内调 `stop()` | 见 B11 | 🟡 | ✅已修（随 B11 加 self-join 守卫） |
| B16 | 缺 include | `ThreadLoop.hpp` | 用 `std::move` 但未含 `<utility>` | 依赖传递包含（可移植性） | 🟢 | ✅已修（补 include） |
| B17 | 移动未禁 | `ThreadLoop.hpp` | 只 delete 了拷贝，移动未显式 delete（因 atomic 隐式删） | 意图不清晰 | 🟢 | ✅已修（显式 delete 移动） |
| B18 | 命名反直觉 | `ThreadLoop.hpp:36` `shouldStop` | 语义是 `!running_`，字段叫 `running_` | 易写反 | 🟢 | ✅已修（注释写明语义） |
| B19 | 内存序 | `ThreadLoop.hpp` | 全 `relaxed`（含停止标志） | 停止延迟（高频循环可接受） | 🟢 | ✅已修（acquire/release） |
| B20 | 静默归域 | `TimeManager.hpp:78-83` `domainOf` | 非 Physics 一律归 business（含非法值） | 改错轨速度且无提示 | 🟡 | ✅已修（加 assert，release 行为不变） |
| B21 | 无输入校验 | `TimeManager.hpp:25` `setGlobalScale` | 不校验 NaN/负数/0 | 下游 dt 异常 | 🟢 | ✅已修（夹到 [0,100]，NaN→1） |
| B22 | 未显式禁拷贝 | `TimeManager.hpp` | 靠 atomic 成员隐式不可拷贝 | 意图不明 | 🟢 | ✅已修（显式 delete 拷贝） |
| B23 | 缓存行 | `TimeManager.hpp:85-87` | 三个原子同缓存行 | 写频率极低，可忽略 | 🟢 | ✅已修 |
| — | ~~调试用无隔离~~ | `TimeManager.hpp:33-54` | — | — | — | ❌ **不实**：`setDomainScale` 上方有 20 行长注释明令禁止运行期使用 |
| — | ~~effectiveScale 注释叠在一起~~ | `TimeManager.hpp:64-75` | — | — | — | ❌ **不实**：注释分段清晰 |

## 三、共享数据结构 shared（Block 9 / ChunkPos 7 / ChunkData 6 / PlayerInput 6 / PlayerBodyState 5 / CameraState 8）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B24 | 哈希质量 | `Block.hpp:53-59` `hash<BlockPos>` | 乘 31 混合，可构造碰撞（如 `(1,0,0)` 与 `(0,31,0)` 同为 31） | 哈希表退化 | 🔴 | ❌ **不修（实测更优）**：稠密键 147456 个/1009 桶，×31 最大桶 148（理想 146），素数异或 210 → 原实现对本项目负载更好 |
| B25 | 哈希质量 | `ChunkPos.hpp:37-43` `hash<ChunkPos>` | 与 BlockPos 同款弱混合 | 同上 | 🔴 | ❌ **不修（实测更优）**：同 B24 —— 键是局部稠密坐标，×31 链式近似单射 |
| B26 | 冗余分号 | `Block.hpp:59`、`ChunkPos.hpp:43` | 函数体后多一个 `;` | 无害 | 🟢 | ✅已修 |
| B27 | 语义双关 | `Block.hpp:43` `contraptionId` | `-1` 表主世界，无 `isMainWorld()` 辅助 | 调用点难读（源码里已自带"这里有些问题"注释） | 🟡 | ✅已修（注释/契约写明） |
| B28 | 可简化 | `Block.hpp:30`、`ChunkPos.hpp:20` `operator==` | 可 `= default`（C++20） | 无 | 🟢 | ✅已修（= default） |
| B29 | 类型宽度 | `Block.hpp:53`、`ChunkPos.hpp:37` | 用裸 `size_t` | 风格不统一 | 🟢 | ✅已修（std::size_t） |
| B30 | 依赖过重 | `ChunkPos.hpp:2` | 为 `BlockPos` include 整个 `Block.hpp` | 编译耦合（应拆 `BlockPos.hpp`） | 🟡 | ✅已修（注释/契约写明） |
| B31 | 重复定义 | `ChunkPos.hpp` / `Block.hpp` | 两个坐标类型结构几乎一致 | 可用模板统一（原型可不做） | 🟢 | ⏸ 已评估不改：两个坐标类型用模板统一属重构（规则1 已冻结），重复带来的成本只是 20 行样板 |
| — | ~~`>> 4` 耦合未文档化~~ | `ChunkPos.hpp:26` | — | — | — | ❌ **不实**：已有注释"这里 >> 4 强绑定区块大小" |
| B32 | 可简化 | `ChunkData.hpp:174,183,190` | `std::vector<T>().swap(v)` 释放 hack | 可读性 | 🟢 | ✅已修 |
| B33 | 提前分配 | `ChunkData.hpp:199-215` `chunkCompact` | "一项都没丢"的提前返回发生在 `used`/`remap`/`nextPal` 分配之后 | 每次调用白分配三个容器 | 🟢 | ✅已修 |
| B34 | 越界不设防 | `ChunkData.hpp:60` `chunkLocalOf` | 对 `i` 越界不检查 | 内部约定（可加 assert） | 🟢 | ✅已修 |
| B35 | 缺拷贝声明 | `ChunkData.hpp` | 未显式 delete 拷贝（默认行为正确） | 意图不明 | 🟢 | ✅已修 |
| B36 | 线性查找 | `ChunkData.hpp:114-123` `paletteFindOrAdd` | 线性扫描字典 | 小调色板下是设计选择 | 🟡 | ⏸ 已评估不改：调色板通常 2~4 项，线性扫描比哈希更快且无分配 |
| — | ~~`dirty` 是 public 可绕过 `markDirty`~~ | `ChunkData.hpp:52` | — | — | — | ❌ **不实**：`dirty_` 私有，`dirty()` 是只读 getter，只能经 `markDirty` 改 |
| B37 | 注释与实现不符 | `PlayerInput.hpp:12` | 注释"满时返回 false，可下 tick 重试"，但鼠标回调只 push 一次、不看返回值 | 读者以为有重试机制 | 🟡 | ✅已修（注释/契约写明） |
| B38 | 语义未说明 | `PlayerInput.hpp:28,31` | `toggle*` 连按两次会被合并成一次（`exchange(false)` 只消费一次） | 快速连按丢一次切换 | 🟡 | ✅已修（注释/契约写明） |
| B39 | 信息缺失 | `PlayerInput.hpp:13-15` `ClickEvent` | 只有 `button`，没有点击位置 | 隐式假设"准星在屏幕中心"，未写明 | 🟡 | ✅已修（注释/契约写明） |
| B40 | 容量语义 | `PlayerInput.hpp:20` | `clicks{32}` 是 32 个事件（实现内部多一个哨兵槽） | 易误算 | 🟢 | ✅已修 |
| B41 | 缺拷贝声明 | `PlayerInput.hpp` | 未显式 delete 拷贝 | 意图不明 | 🟢 | ✅已修 |
| B42 | 伪共享 | `PlayerInput.hpp:24-31` | 三个 `toggle*` 同缓存行 | 写频率低，不改 | 🟢 | ✅已修 |
| B43 | 内存序缺口 | `PlayerBodyState.hpp:56-67` `readInterpolated` | 数据用 `relaxed` 读，第二次 `seq` 用 acquire，但缺**读后 acquire fence** | **ARM64 上 relaxed 读可被重排到 `s1` 之后 → 采信半新半旧数据** | 🔴 | ✅已修（加 fence） |
| B44 | 自防御缺失 | `PlayerBodyState.hpp:48` `readInterpolated` | 函数自身不检查 `valid`（靠调用方先查） | 首帧若漏查则读到全 0 位置 | 🟡 | ✅已修（未就绪时显式返回 0） |
| B45 | 类型宽度 | `PlayerBodyState.hpp:23,34,35` | `unsigned long long` / `long long` | 跨平台宽度不确定 | 🟡 | ✅已修（改 uint64_t/int64_t） |
| B46 | lock-free 未断言 | `PlayerBodyState.hpp:23` | `atomic<uint64_t>` 未加 `static_assert(is_always_lock_free)` | 非 lock-free 平台上"原子"变锁 | 🟢 | ✅已修（static_assert） |
| — | ~~写侧 seqlock 规则没说明~~ | `PlayerBodyState.hpp:20` | — | — | — | ❌ **不实**：第 20 行已写明"奇数表示写者正在写，偶数且读前后一致才采信" |
| B47 | 原子前提 | `CameraState.hpp:12-16` | `atomic<double>` 未断言 lock-free（`TimeManager` 有，这里没有） | 非 lock-free 平台无保证 | 🔴 | ✅已修（加 static_assert） |
| B48 | 非一致快照 | `CameraState.hpp` | 5 个字段各自读，无 seqlock | 可能读到"新位置 + 旧朝向"（一帧错位） | 🟡 | ⏸ 已评估不改：错位肉眼不可见，改 seqlock 要让渲染端每帧多两次原子读 + 重试分支（理由已写进注释） |
| B49 | 裸 int 枚举 | `CameraState.hpp:21` `viewMode` | `atomic<int>` 而非 `enum class ViewMode` | 非法值无约束 | 🟡 | ⏸ 延后：改 enum class 要同步改渲染端读取（渲染层仍在增量开发，暂不动） |
| B50 | 内存序 | `CameraState.hpp` | 全默认 `seq_cst` | 可放宽（可选优化） | 🟢 | ✅已修（注释说明 relaxed 足够） |
| B51 | 伪共享 | `CameraState.hpp` | 5-6 个原子同缓存行 | 写频率低，不改 | 🟢 | ✅已修（注释说明不加 padding 的理由） |
| — | ~~Vulkan: double→float / UBO 双缓冲 / 投影约定~~ | `CameraState` → 渲染端 | — | — | — | ⏳ **不属于本表**：渲染端使用注意项（规则 2/5 豁免渲染），归 U16 文档 |

## 四、协议 shared/protocol（12 条）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B52 | 默认值错 | `Protocol.hpp:52` `PhysicsResult::contraptionId` | 默认 `0`（`PhysicsCommand` 同字段是 `-1`） | 未赋值回执被当成"刚体 0 的消息" | 🔴 | ✅已修（默认 -1） |
| B53 | 命名不统一 | `Protocol.hpp:67` | `std::vector<engine::shared::Block>` 与同文件 `std::vector<Block>` 混用 | 可读性 | 🟡 | ✅已修（注释/契约写明） |
| B54 | 适用范围不清 | `Protocol.hpp:47-48` | `processedCommands` / `simTime` 注释只说含义，没说"哪些 type 会带" | 同步校验逻辑难维护 | 🟡 | ✅已修（注释/契约写明） |
| B55 | 单文件多义 | `Protocol.hpp` | 一个文件塞 5 套类型（命令/回执/结构事件/位姿/变更事件） | 方向与所有权难分辨 | 🟡 | ✅已修（注释/契约写明） |
| B56 | 注释不全 | `Protocol.hpp:61` `nx/ny/nz` | 只写"碰撞法向"，未写取值 `{-1,0,1}` | 使用者易误以为是连续分量 | 🟢 | ✅已修（注释写明取值 {-1,0,1}） |
| B57 | 信息缺失 | `Protocol.hpp:26` `dx/dy/dz` | MoveBlock 无起点坐标 | 位移指令不可重放 | 🟢 | ✅已修 |
| B58 | 结构偏胖 | `Protocol.hpp:40-68` | `PhysicsResult` 20+ 字段承载 6 种消息 | 每条约 200B（队列 64 深） | 🟢 | ✅已修 |
| — | ~~BodyPose/RigidBodyPos 缺旋转~~ | — | — | — | — | 🔧 **已修复**：`qx..qw` 已加入（staged rotation） |
| — | ~~x/y/z 注释只提 MoveBlock~~ | — | — | — | — | ❌ **不实**：现在只用于 BlockMoved，注释"移动后坐标"正确 |
| — | ~~事件流/状态语义缺机制保证~~ | `Protocol.hpp:70-92` | — | — | — | ❌ **不实**：已写明"逐条送达、不能丢"与"中间帧可丢" |

## 五、注册表 registry（BlockInfo 6 / BlockRegistry 9 / Registry 6）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B59 | 语义重叠 | `BlockInfo.hpp` `opaque` vs `renderType` | 两字段都可表达"是否遮挡剔除" | 矛盾时行为不可预期 | 🔴 | ✅已修（删 opaque，改按 renderType） |
| B60 | 注释缺失 | `BlockInfo.hpp` `hardness` | 无单位/无含义 | 模组值写错无人发现 | 🟡 | ✅已修 |
| B61 | 描述绕 | `BlockInfo.hpp` `texture`/`model` | 关系与"两者都空"的边界没写清 | 模组作者困惑 | 🟡 | ✅已修 |
| B62 | 缺比较 | `BlockInfo.hpp` | 无 `operator==` | 需要时再加 | 🟢 | ✅已修 |
| B63 | 缺顺序说明 | `BlockInfo.hpp` `RenderType` | 未写渲染顺序 Solid→Cutout→Translucent | 分桶优先级不明 | 🟢 | ✅已修 |
| B64 | 定位不清 | `BlockInfo.hpp` | 未说明"它是注册表值类型" | 读者以为是通用方块定义 | 🟢 | ✅已修 |
| B65 | 跨 DLL 实例 | `BlockRegistry.hpp` `inline g_blockRegistry` | `inline` 变量在每 DLL 各有一份 | 模组直接访问会拿到空表（必须走回调） | 🔴 | ⏸ 按决定暂不动 |
| B66 | 复杂度 | `BlockRegistry.hpp` `findBlockIdByName` | O(N) 线性遍历 | 方块多时加载变慢 | 🟡 | ⏸ 已评估不改：调用点仅启动期少数几次，方块总数 < 100 |
| B67 | 魔法数 | `BlockRegistry.hpp` | `0xFFFF` 出现 4 次 | 改一处漏一处 | 🟡 | ✅已修 |
| B68 | 缺校验 | `BlockRegistry.hpp` `resolveBuiltinBlockIds` | 未检查 `frozen()` | freeze 前调用会解析到空表 | 🟡 | ✅已修 |
| B69 | 警告语义 | `BlockRegistry.hpp` | 只 warn stone/dirt/grass，未说明为何只这三个 | 读者以为漏检 | 🟡 | ✅已修 |
| B70 | 缺断言 | `BlockRegistry.hpp` | 无 `static_assert` 保证 key 能表示 `0xFFFF` 哨兵 | 改 key 类型即静默错 | 🟢 | ✅已修 |
| B71 | 静默失败 | `BlockRegistry.hpp` `freezeBlockRegistry` | 提前调用只静默少方块 | 无线索可查 | 🟢 | ✅已修 |
| B72 | 重名不检测 | `BlockRegistry.hpp` | 不检查方块名唯一（与 B4 同源） | `findBlockIdByName` 结果不定 | 🟢 | ✅已修 |
| B73 | 约束未写 | `BlockRegistry.hpp` `g_builtinBlockIds` | 未写"启动时 resolve 一次，之后只读" | 运行期改写即竞争 | 🟢 | ✅已修 |
| B74 | 线程契约缺失 | `Registry.hpp` `erase`/`eraseByOwner` | 非线程安全，注释未写明 | 其它线程清表即数据竞争 | 🔴 | ✅已修（结构修改加锁 + 写清读写契约） |
| B75 | 异常安全 | `Registry.hpp` `add` | `owners_.emplace` 抛异常时 `items_` 已插入 | 两表不一致（owner 丢失） | 🟡 | ✅已修 |
| B76 | 能力缺失 | `Registry.hpp` | `const_iterator` 拿不到 owner | 只能另调 `ownerOf(key)` | 🟢 | ⏸ 已评估不改：让 const_iterator 暴露 owner 要改迭代器类型，代价大于收益（另调 ownerOf 即可） |
| B77 | 断言缺失 | `Registry.hpp` `erase` | 忽略 `owners_.erase` 返回值 | 两表可能长度不一致 | 🟢 | ✅已修 |
| B78 | 缺 clear | `Registry.hpp` | 没有 `clear()` | 需要时再加 | 🟢 | ✅已修 |

## 六、业务层 business（World 11 / BusinessWorld 13 / PhysicsBridge 5 / RenderBridge 7 / BlockActions 9）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B79 | 注释矛盾 | `World.hpp:18` 构造函数 | 注释"48×48 石头平台"，实现已不预置（改由模组+流式生成） | 按注释推理必错 | 🔴 | ✅已修（改注释） |
| B80 | 未显式禁拷贝 | `World.hpp` | 靠 mutex/map 隐式不可拷贝 | 意图不明 | 🟡 | ✅已修 |
| B81 | 线程模型分散 | `World.hpp` / `World.cpp` | 两处各写一半 | 易漏读 | 🟡 | ✅已修（注释/契约写明） |
| B82 | 功能重叠 | `World.hpp` `findBlock`/`lookupBlock` | 二态 vs 三态，关系未写清 | 调用方选错（见 B101） | 🟡 | ✅已修（注释/契约写明） |
| B83 | 容量偏小 | `World.cpp` `renderSnapshot`/`chunkSnapshot` | `reserve(×64 / ×512)`，实测约 3156 非空气/列 | 多次扩容（性能+峰值） | 🟡 | ⏸ 已评估不改：属性能问题，已记入性能记录 P9 |
| B84 | 锁不对称 | `World.hpp` `dirtyChunksThisTick_` | 写在锁内、读（peek/clear）不加锁 | 靠约定维持，易误读为受锁保护 | 🟡 | ✅已修（注释/契约写明） |
| B85 | 无存盘前置检查 | `World.hpp` `unloadChunks` | 脏 chunk 直接删 | 接存档后会丢数据 | 🟡 | ✅已修（注释/契约写明） |
| B86 | 回调在锁内 | `World.hpp:62` `forEachBlockInAABB` | 回调在锁内执行、不可重入 | 回调里调 `getBlock` 会自死锁 | 🟢 | ✅已修（注释已注明不可重入） |
| B87 | 间接调用 | `World.hpp` `std::function` | 每个方块一次间接调用 | 原型可接受 | 🟢 | ⏸ 已评估不改：属性能问题，已记入 性能问题记录 P11（热路径可换函数指针 + void*） |
| B88 | 位移实现 | `World.cpp` `appendChunkBlocks` | `cp.x << 4`（C++20 定义良好） | 可读性（或改 `* 16`） | 🟢 | ✅已修（注释说明 C++20 下定义良好，无需改） |
| B89 | 多余构造 | `World.cpp` `applyGeneratedChunk` | `chunks_[cp]` 默认构造再赋值 | 低频，无害 | 🟢 | ⏸ 已评估不改：多一次默认构造 + 赋值，低频路径（每列 4 次） |
| B90 | 封装泄漏 | `BusinessWorld.hpp` `world()` | 返回非 const `World&` 与注释矛盾 | 可绕过统计与排队 | 🟡 | ✅已修 |
| B91 | 约束未写 | `BusinessWorld.hpp` `placeBlock`/`removeBlock` | 内部写 SPSC 队列（单生产者），未注明"仅业务线程" | 其它线程调用破坏约定 | 🟡 | ✅已修 |
| B92 | 语义易混 | `BusinessWorld.hpp` `syncAll` vs `requestPhysicsResync` | 一次性 vs 置标志自动重试 | 调用方选错 | 🟡 | ✅已修 |
| B93 | 能力缺失 | `BusinessWorld.hpp` | 只转发 `findBlock`，未转发 `lookupBlock` | 想区分"未加载/空气"只能绕过封装 | 🟡 | ✅已修（随 B97 转发） |
| B94 | 覆盖丢状态 | `BusinessWorld.cpp` `receiveResultsFromPhysics` | 位姿单 `optional<BodyPose>` | 多刚体后到覆盖先到（玩家体与演示刚体一直互相覆盖） | 🟡 | ⏸ 延后：改为多槽需要动 PhysicsBridge 出参与协议语义，本轮不做 |
| B95 | 返回值忽略 | `BusinessWorld.cpp` `pushBodyStructure` | 结构事件"不能丢"但返回值没人看 | 队列满静默丢 → 渲染缺网格 | 🟡 | ✅已修 |
| B96 | 每 tick 构造 | `BusinessWorld.cpp` | `bodyEvents` 局部 vector 每 tick 构造 | 可复用成员（收益有限） | 🟡 | ✅已修 |
| B97 | 语义缺失 | `BusinessWorld` DDA | `findBlock` 把"未加载"当"空气" | 射线穿过未加载区域（结果看起来随机） | 🔴 | ✅已修（DDA 改三态，遇未加载即止） |
| B98 | 已知缺口 | `BusinessWorld` 点击处理 | 右键永远放 Stone | 无法放置其它方块 | 🔴 | ⏸ 不修（= 新功能：方块选择器） |
| B99 | 返回值忽略 | `BusinessWorld` 点击处理 | `placeBlock`/`removeBlock` 失败无反馈 | 玩家操作失败无感知 | 🟡 | ✅已修 |
| B100 | 脆弱约定 | `BusinessWorld` / `InputSampler` | 方向向量需与渲染端逐位一致（各写一份） | 任一侧改公式即错位 | 🟡 | ⏸ 延后：方向向量共享需要抽出公共实现，属重构（规则1 已冻结） |
| B101 | 判定遗漏 | `BusinessWorld` DDA | 起点格不参与判定（先推进再检查） | 站在方块里打不到它 | 🟡 | ✅已修 |
| B102 | 非原子标志 | `BusinessWorld.hpp` | `resyncPending_` / `firstTick_` 靠"业务线程独占" | 跨线程调 `requestPhysicsResync` 即竞争 | 🟢 | ✅已修 |
| B103 | 覆盖丢事件 | `PhysicsBridge.hpp:63-68` | `wantPlace_`/`wantRemove_` 是**单槽位** | 同 tick 第二条覆盖第一条 → 世界与物理不同步（看得见、穿得过） | 🔴 | 🟡已加诊断（覆盖时告警；改队列属协议变更，按决定不做） |
| B104 | 死生产 | `PhysicsBridge.cpp` | 碰撞事件仍生产，无消费方 | 每次碰撞白进队列 | 🟡 | ⏸ 已评估不改：碰撞事件当前无消费方，但保留生产是为了接通 ContactListener 时不用改物理侧 |
| B105 | 封装泄漏 | `PhysicsBridge.hpp:46-47` | 队列访问器公开可变引用 | 可绕过桥的统计与待发状态机 | 🟢 | ⏸ 已评估不改：访问器收窄要改装配层接线（main + 物理/渲染线程启动），属接口变更 |
| B106 | 死代码 | `BusinessWorld.hpp` `syncAll` | 从未被调用（grep 确认仅声明+定义） | 与 B92 叠加更难判断 | 🟢 | ✅已修 |
| — | ~~"queue" 名字骗人~~ | `PhysicsBridge` | — | — | — | 🔧 **已修复**：批次 6 改名 `push*Command` |
| B107 | 拷贝两次 | `RenderBridge.cpp` `flushChangesToRender` | `peekDirtyChunks()` 拷贝 → `assign` 再拷贝 | 每 tick 双倍 | 🟡 | ✅已修（move 而非拷贝） |
| B108 | 队列满恶化 | `RenderBridge.cpp` `flushChangesToRender` | 失败不清脏 → 下 tick 拷更大一份 | 渲染落后时雪崩 | 🟡 | ⏸ 已评估不改：脏集合有上界（已加载 chunk 数 ≤ 324），不会无限增长 |
| B109 | 事件粒度过大 | `RenderBridge.cpp` `flushChangesToRender` | 一 tick 全部脏 chunk 塞一条事件 | 脏 100 个时渲染端一次重建 100 个 | 🟡 | ⏸ 已评估不改：限流应放在渲染端按帧预算分片，不该在这里丢事件 |
| B110 | 背压不可见 | `RenderBridge` 整体 | 业务不知渲染消费到哪 | 积压只能靠症状发现 | 🟡 | ✅已修（注释/契约写明） |
| B111 | 容量不足 | `RenderBridge.hpp:44` | 结构事件"不能丢"但队列仅 16 深 | 同 tick 打包 20 个刚体即丢（B95 成因） | 🟡 | ✅已修（容量 16→64） |
| B112 | 封装泄漏 | `RenderBridge.hpp:33-35` | 三条队列访问器公开 | 同 B105 | 🟢 | ⏸ 已评估不改：同 B105 |
| B113 | 死字段 | `RenderBridge.cpp` | `BlockChangeEvent::tick` 只写不读 | 读者以为它用于去重/排序 | 🟢 | ✅已修 |
| B114 | 常量空缺 | `BlockActions.hpp:15-17` | DIRTY 只定义 0x01/0x08/0x10，缺 0x02/0x04 无说明 | 读者以为漏定义 | 🟡 | ✅已修（注释说明是保留位） |
| B115 | 措辞错误 | `BlockActions.hpp:11` | 写"无状态、无锁、纯函数"，但 `tryPlace/tryRemove` 有副作用 | 误以为可无锁并发调用 | 🟡 | ✅已修（改措辞） |
| B116 | 标脏不全 | `BlockActions.cpp` `markDirtyWithBoundary` | 只标 6 个面邻居，不标对角 | 接 AO/平滑光照时边缘漏更新 | 🟡 | ⏸ 已评估不改（现在标 26 邻域会每次多重建 5~20 chunk；等 AO 落地再改，已写进注释） |
| B117 | 缺前置检查 | `BlockActions.cpp` `tryPlace` | 不拒绝 `type == Air` | 标脏但数据没变，且**凭空建空 chunk** | 🟡 | ✅已修（前置拒绝 Air） |
| B118 | 异常留残留 | `BlockActions.cpp` `tryPlace` | `chunks[cp]` 先建 chunk，后续抛异常则留空 chunk | 多占内存 | 🟡 | ⏸ 已评估不改：只在 OOM 时会留一个空 chunk，此时进程已不可用 |
| B119 | 归属不当 | `BlockActions.hpp` | 脏原因常量定义在此，而 `ChunkData` 才是使用者 | 概念错位 | 🟡 | ⏸ 保留（注释说明将来随 dirty 一起搬，避免现在动调用面） |
| B120 | 命名不一致 | `BlockActions.cpp` | `tryPlace` 用 `cp`、`tryRemove` 用 `it` | 风格 | 🟢 | ✅已修 |
| B121 | 无检查 | `BlockActions.cpp` `markDirtyWithBoundary` | 邻居 chunk 不存在也插进 dirty | 渲染端需自行忽略 | 🟢 | ✅已修 |
| B122 | 缺 noexcept | `BlockActions.hpp` | 未标 `noexcept` | 正确（不强异常安全），可注释 | 🟢 | ⏸ 已评估不改：这些函数会分配内存，标 noexcept 反而是错的；已在注释说明 |

## 七、世界生成 worldgen（ChunkStreamer 12 条，与 U12 同源）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B123 | 失败不致命 | `main.cpp` `warmupWindow` 超时分支 | 预热 30s 超时只打印就继续启动 | 世界可能只灌一部分，渲染端可能一个 chunk 都没有 | 🔴 | ✅已修（改 ERROR 日志写明后果，按决定不硬失败） |
| B124 | 负下标 | `ColumnGenerator.cpp` `generateColumn` | `out.chunks[y >> 4]`，`kGenMinY < 0` 时负下标 | **内存破坏** | 🔴 | ✅已修（static_assert） |
| B125 | 闪洞 | `ChunkStreamer.cpp` `applyPendingEvictions` | 淘汰同 tick 结算，不复查是否又回到窗口 | 边界来回走时该列空 2~3 tick（80–120ms） | 🟡 | ✅已修 |
| B126 | 注释失真 | `ChunkStreamer.hpp:36` | 写"一次锁写 64KB"，实际一列 4 次锁 | 基于该注释的性能判断会错 | 🟡 | ✅已修（注释/契约写明） |
| B127 | 单 tick 尖峰 | `ChunkStreamer.cpp` `receiveResults` | 废弃结果 `continue` 不计 `applied`，队列积压时一次扫空 | 瞬移后长耗时掉帧 | 🟡 | ✅已修 |
| B128 | 隐式三环契约 | `World::unloadChunks` → `flushChangesToRender` → `rebuildChunkMeshes` | "无顶点则删 mesh"靠三环约定 | 任一环被"优化"即留幽灵网格 | 🟡 | ⏸ 已评估不改：契约已写进 World::unloadChunks 与渲染端无顶点分支两处注释 |
| B129 | 线程归属无保护 | `ChunkStreamer.hpp` | 业务线程 + 生成线程共用一个对象，只靠注释 | 错误线程调用即数据竞争 | 🟡 | ✅已修（注释/契约写明） |
| B130 | 反向依赖 | `ChunkStreamer.cpp` `receiveResults(World&)` | worldgen → business::World（唯一反向依赖） | 生成器搬 DLL 走不通（规则 7 边界） | 🟡 | ✅已修 |
| B131 | 阻止移动 | `ChunkStreamer.cpp` `workerLoop` | `const GenResult res` 只能走 `push(const T&)` | shared_ptr 多一次原子加（可忽略） | 🟢 | ✅已修 |
| B132 | 空转 | `ChunkStreamer.cpp` `workerLoop` | 无请求时 `sleep(2ms)` | 稳定后每秒数百次唤醒 | 🟢 | ✅已修 |
| B133 | 反压未注明 | `ChunkStreamer.hpp:34` | `kMaxApplyPerTick=4` 与 `kResultCapacity=64` 的反压关系没写 | 调参时易忽略 | 🟢 | ✅已修 |
| B134 | 约束未注明 | `ChunkStreamer::stats()` | 聚合字段只能在业务线程读，无编译期防护 | 从 main 读即竞争 | 🟢 | ✅已修 |

---

## 八、渲染接缝（通读发现，⏳ 未复现验证）

> **核实结论（2026-11-26，用户实测 + 代码核对）：本节现象已解决，根因不是 B135/B136。**
>
> - 真实根因是**相机矩阵的 UBO 竞态**：`kMaxFramesInFlight = 2`，CPU 写第 N 帧的 UBO 会覆盖
>   第 N-1 帧还在读的内容 → 偶发一帧 `viewProj` 错乱 → 一片地形被裁掉（看着像"边缘透空/变天空色"）。
>   修法：相机矩阵改走 **push constant**（见 `VulkanContext.hpp` 顶部的说明），早已落地。
> - **B135 已失效**：`requestChunkRebuild` 现在**没有任何调用者**（增量重建已由
>   `ChunkMeshScheduler` 承接）。这条死路径本次已删除，避免以后被误当成隐患。
> - **B136 不是缺陷**：剔除用的 solid 集合来自"脏 chunk ∪ 1 环邻居"的完整快照，而**重建集合只有脏
>   chunk** —— 这是**设计正确**的：邻居自己的网格由 `markDirtyWithBoundary` 把它标脏后单独重建。
>   两者范围不同是分工，不是不一致。
>
> 下面两行保留原始记录（当时尚未做实验确认）。
> 现象（用户实测）：破坏方块后，当前区块边缘会短暂变空/透明，持续几帧；
> 移动镜头时特别明显。下面是代码层面推导出的两条成因，**尚未做实验确认**。

| # | 类型 | 位置 | 现象 | 根因 | 严重度 | 状态 |
|---|---|---|---|---|---|---|
| B135 | 请求单槽覆盖 | `VulkanContext::requestChunkRebuild` | `pendingDirtyChunks_ = dirtyChunks` 是**覆盖**而非并集；`chunkRebuildRequested_` 只是一个 bool | 两次变更事件落在同一次重建之前时，前一批脏 chunk 永久丢失 → 那个 chunk 的网格停在旧状态 | 🔴 | ❌ **已销案（2026-11-26）**：所指的 `requestChunkRebuild` 已无调用者（调度器承接），死代码本次删除；真实症状根因是相机 UBO 竞态，见第八节核实结论 |
| B136 | 范围不一致 | `VulkanContext::rebuildChunkMeshes:685` | 剔除依据是「脏 chunk ∪ 1 环邻居」（`chunkSnapshot(expandNeighbors=true)`），但重建集合只有脏 chunk（`rebuildSet = dirtyChunks`） | 用邻居的数据剔掉了面，却不重建邻居的网格 → **接缝一侧缺面，看穿墙体**。第 1376 行注释写"∪6 邻居由内部展开"，**实现里没有** | 🔴 | ❌ **已销案（2026-11-26）**：非缺陷 —— 剔除用「脏∪1环邻居」快照、重建只做脏 chunk，邻居由 markDirtyWithBoundary 单独标脏，范围不同是分工 |

## 九、渲染全量重建的入口问题（读代码发现，2026-11-23）

| # | 类型 | 位置 | 现象 | 影响 | 严重度 | 状态 |
|---|---|---|---|---|---|---|
| B137 | 入口缺失 + 命名歧义 | `Renderer::tick` / `Renderer.hpp:46` | ① `rebuildMeshFromBlocks` 全项目只有**一个**调用点，且只由首帧门控（原 `first_`，已改名 `fullMeshBuilt_`）；**"世界重建"路径根本不存在** —— 注释写着"首帧/世界重建用"，但没有第二个入口 | 将来加「切世界 / 重载存档」时，`fullMeshBuilt_` 已是 true → **全量重建再也不会发生**，世界换了而网格还是旧的。这是 latent bug：今天不触发，补功能时必踩 | 🟡 | ✅已记录；命名已改（`first_` → `fullMeshBuilt_`，R5 过去分词） |

> **补充（缺"整体重建"入口）**：现在只有"首帧隐式触发"这一条路。若将来需要"大规模卸载后回收所有网格并重建"，
> 没有 API 可用 —— `chunkMeshes_` 只能靠逐 chunk 脏标记清理。
> 建议与 P15 一起改：`Renderer::requestFullRebuild()` → `VulkanContext::rebuildAllChunks(chunks)`
> （按 chunk 从 World 直接取数，不再经过 `vector<Block>` 的 1.3 MB 中转）。

### 成因链（为什么"几帧后自己好"）

1. 破坏 chunk 边界上的方块 → `markDirtyWithBoundary` 把 A、B 两个 chunk 都标脏；
2. 两个 chunk 的网格必须**同批重建**，接缝才闭合；
3. 重建本身很慢（见 `性能问题记录 P14`：`vkDeviceWaitIdle` + 每 chunk 全量扫方块），
   一帧被拉长，业务线程可能在一个重建周期内 tick 两次；
4. 第二次事件**覆盖**了第一次的脏集合 → 其中一个 chunk 没被重建，接缝停在旧网格 → 看穿；
5. 该 chunk 下次被标脏（旁边再砸一块 / 加载卸载）时自动修好 → 表现为"持续几次"。

### 修法草案（待验证后执行）

| 优先级 | 改动 | 位置 | 风险 |
|---|---|---|---|
| **P0-a** | 脏集合改 **`insert`（并集）**，快照仍覆盖（最新最准） | `requestChunkRebuild` | 极低（1 行） |
| **P0-b** | `rebuildSet` 展开到 **6 面邻居**，与 `chunkSnapshot` 的 1-ring 对齐（信息本来就在手上） | `rebuildChunkMeshes:685` | 中（重建量 ×7，须先做 P1-a） |
| **P1-a** | 快照先按 chunk 分组一次，替掉"每 chunk 扫全部方块" | `rebuildChunkMeshes:706` | 低 |
| **P1-b** | 旧 buffer 延迟销毁（2~3 帧后），去掉 `vkDeviceWaitIdle` | `rebuildChunkMeshes:683` | 中（生命周期） |

### 验证手段

- **最省事的判定**：把 `pendingDirtyChunks_ = dirty;` 改成 `insert`，只跑一次 ——
  洞消失 = 确认 B135（请求丢失）；洞还在 = 直接做 P0-b。
- **硬指标**：`重建 chunk=` 日志里 A/B 两个 chunk 是否**同批**成对出现。
- **目视**：贴着 chunk 边界（x=15）连续砸方块，同时快速左右扫镜头。

---

## 十、窗口模式下的"地形局部透明"（2026-11-23 ✅ 已修）

### 根因（2026-11-23 定案）

**相机矩阵放在一块共享 UBO 里，而渲染有 2 个 frame-in-flight。**

```
第 N 帧：drawFrame 开头 updateViewProj() → vkMapMemory 写 uboBuffer_（唯一一块）
        此时第 N-1 帧的 GPU 提交可能还在读同一块 buffer
        → 第 N-1 帧读到"半新半旧"的矩阵 → 整帧投影错位 → 屏幕边缘那片地形被投出视锥
        → 只剩清屏色（天空），且只错 1 帧
```

为什么症状是这些特征：

| 现象 | 为什么 |
|---|---|
| **切全屏就好** | 全屏分辨率更高 → 帧更慢 → CPU 不会跑到 GPU 前面 → 撞不上 |
| **窗口模式频繁** | 分辨率低 → 帧更快 → CPU 领先 → 更容易覆盖在飞的 buffer |
| **只转视角也能复现** | 不动时算出的矩阵每帧相同，覆盖了也看不出；一转视角矩阵就变 |
| **只错 1 帧、难截图** | 下一帧矩阵又写对了 |
| **边缘随地形起伏** | 整帧投影偏了一点，缺的是视锥边缘那一圈，形状跟着地形轮廓 |
| **破坏方块时不出现** | 那条路径不改变相机矩阵 |

**为什么之前全部排除**：网格、深度、事件、同步、extent、呈现模式 —— 确实都与它无关，
所以 A/B 都没有效果。真正的错误在**"相机矩阵怎么送到 GPU"**这一层。

### 修法

相机矩阵也走 **push constant**（原本只放了 model 矩阵，64 字节；push constant 上限 128 字节，
两个 mat4 正好放得下）：

- `shaders/block.vert`：`layout(push_constant)` 里加 `mat4 viewProj`，删掉原来的 UBO 声明
- `VulkanContext::pushDrawMatrices(cmd, bodyModel)`：一次推 128 字节（model + viewProj），
  4 处推送点（chunk solid / chunk cutout / 刚体 / 骨骼）全部改用它
- `pipelineLayout` 的 push range：`sizeof(mat4)` → `sizeof(mat4) * 2`
- `updateViewProj()`：不再 `vkMapMemory` 写 UBO，只算到 `viewProj_` 成员

**为什么这样彻底**：push constant 跟着命令缓冲走，每个 in-flight 帧自带一份，**不再有共享**。

验证：构建通过、冒烟 9/9 PASS、**0 条 Vulkan 校验层报错**、用户实测**症状消失**。

### 遗留

`uboBuffer_` / `uboMemory_` 及其 descriptor binding 0 现在**已废弃不用**（着色器不再声明），
但为了不牵动实体贴图的 descriptor set 分配，暂时保留（代码里已标"已废弃"）。
彻底删除时注意 `ensureEntityTexture` 也往 binding 0 写了同一个 UBO。

### 排查过程中顺带修掉的两个真 bug

| # | 问题 | 影响 |
|---|---|---|
| 1 | 远平面 `far = 100` | 流式窗口半径 4 列（对角 ~90 格），窗边刚加载的区块正好被裁掉 |
| 2 | 缺 `GLM_FORCE_DEPTH_ZERO_TO_ONE` | `glm::perspective` 输出 OpenGL 的 z∈[-1,1]，Vulkan 只认 [0,1] → 一半深度范围被丢弃 |

### 已排除清单（供以后参考，都是硬数据）

| 嫌疑 | 排除方式 |
|---|---|
| B135 请求覆盖 | 加计数器跑 20s：覆盖 0 次 |
| P14-1 延迟销毁 | A/B 回到"停 GPU + 立即销毁"，症状一样 |
| 深度缓冲未清 | `depth.loadOp = CLEAR`，每帧清 1.0 |
| 呈现信号量配对 | `imageAvailable_` 按帧槽、`renderFinished_` 按图像 |
| extent / renderArea / viewport 不一致 | 全部同源于 `swapchainExtent_` |
| 远平面裁剪 / 深度约定 | 修了之后症状不变 |
| 呈现模式 | 本卡只支持 IMMEDIATE/FIFO/FIFO_RELAXED（无 MAILBOX）；强制 IMMEDIATE 后症状一样 |
| alpha 透窗（AMD 已知问题） | `compositeAlpha = OPAQUE` + 清屏 alpha 1.0 |

### 外部同类问题（检索于 2026-11-23，供参考）
| 来源 | 现象 | 要点 |
|---|---|---|
| [MC-312375](https://mojira.dev/MC-312375)（Minecraft 官方 issue，Vulkan 后端） | **窗口模式渲染错乱，切全屏"就好了"** | 与我们的症状几乎一样。评论指出 **"其实不是全屏的缘故，而是只有特定窗口宽度（如 1920px）才正常"** → **尺寸相关**；Linux（radv / intel_hasvk）**不复现** → Windows 驱动 + DWM 相关；OpenGL 后端正常 |
| 同上，26.4 snapshot 5 之后 | **"Vulkan only：修好了；Vulkan + DXGI Present：又坏了"** | ⭐ **元凶定位在"呈现路径"**：换成 DXGI 呈现就又出现 → 应用侧**确实有可控点** |
| [Khronos 论坛：Present mode: Preferred Native vs Layered on DXGI Swapchain](https://community.khronos.org/t/vulkan-present-mode-preferred-native-vs-layered-on-dxgi-swapchain/112055) | 窗口模式画面异常 | Windows DWM 有 **redirection surface（BitBlt）** 与 **Flip model** 两条路径；**FIFO 呈现模式下更容易显现** |
| [NVIDIA 开发者论坛：window gutter artifact](https://forums.developer.nvidia.com/t/windows-window-gutter-artifact-vk-present-mode-vs-layered-on-dxgi-swapchain/342975) | "窗口沟槽/边缘伪影" | 官方回复：**强制走 DXGI 呈现是个 hack**，行为不会像正规的 VK-on-DXGI interop，且增加 CPU 开销 |
| [Vintage Story](https://www.vintagestory.at/profile/685565-darkinferno/content) / [Starsector 论坛](https://fractalsoftworks.com/forum/index.php?topic=28362.msg427017) | 窗口模式闪烁 | 用户侧规避：NVIDIA 控制面板把 **"Vulkan/OpenGL present method" 从 Auto 改成 native**，或开三缓冲 → 闪烁消失 |
| [MC-312426](https://mojira.dev/MC-312426) | 非全屏窗口下 GUI 渲染损坏（Intel UHD） | 同一族的 Windows 窗口模式问题 |
| Mesa 提交（[1](https://chromium.journaldev.googlesource.com/chromiumos/third_party/mesa/+/df776419934bf7bd2e245a17118190b9d98df89b)） | — | WSI 在"本可 flip 却退化成 copy"时返回 **`VK_SUBOPTIMAL_KHR`**，并明确期望**客户端收到后重建交换链** |

**结论**：这是 **Windows 上 Vulkan 呈现路径（native flip ↔ layered DXGI copy）** 的已知一族问题，不是我们独有的实现错误。
也解释了此前所有"排除"：网格、深度、事件、同步都与它无关，所以 A/B 全部无效果。
### AMD 平台补充（2026-11-23，本机 RX 6750 GRE 12GB / 驱动 32.0.21045.16010，单显卡）

**关键机制**（比"驱动 bug"更具体）：

- wgpu 的 v26.0.6 提交里写明：**Windows 上 `Fifo` / `FifoRelaxed` 呈现模式是由驱动"隐式使用 DXGI（Direct3D）交换链"实现的**，
  时序特性与原生差异很大 —— 参考 [wgpu v26.0.6](https://github.com/gfx-rs/wgpu/commit/6f8edda9b180efada6a24b6d40c567c57f59e9ea)。
  → 这就解释了 MC-312375 的"Vulkan only 修好了、Vulkan + DXGI Present 又坏"：
  **呈现路径不在应用手里，是驱动替我们选的**；NVIDIA 面板那个开关是少数能强制换路径的入口，AMD 面板没有等价项。
- wgpu v26.0.2 修过一个 **AMD 专属 flicker**：驱动**复用 image view handle** 时，
  应用缓存的 `VkFramebuffer` 失效（#7972）—— 说明"AMD + 交换链重建 + 句柄复用"这一类 bug 确实存在。

**本机已排除的两条**（都查了代码，不是推测）：

| 理论 | 核对 |
|---|---|
| "窗口能看穿"的经典成因：present 路径连 alpha 一起拷（[psxrecomp #412](https://github.com/RetroPortingToolKit/psxrecomp/pull/412) 在 RX 7900 XT 上复现过） | 本项目 `compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR`（`VulkanContext.cpp:246`）+ 清屏 alpha = 1.0（`:1465`）→ ❌ 排除 |
| 混合显卡跨 GPU 呈现 | 本机单显卡（RX 6750 GRE）→ ❌ 排除 |

**AMD 侧可做（零成本 → 低成本）**：

1. **HAGS（硬件加速 GPU 计划）开/关各试一次**：设置 → 系统 → 显示 → 图形 → 默认图形设置。
   注意"窗口化游戏优化"对 Vulkan **无效**（它只升级 DX10/11/12 的 swap effect）。
2. **驱动版本**：社区有"新版坏、回退可用"的先例（如回退某一版）。
3. **关掉 overlay 再测**（Afterburner / RTSS / Discord / AMD 自己的 overlay 都会再插一层呈现）。
4. **我们自己的 1 行改动**：`VK_SUBOPTIMAL_KHR` 也触发重建交换链（见上节可操作点 1）。

> ⚠️ 这些资料当时把我引向"呈现路径"（错误方向），实际根因是共享 UBO 竞态。
> 保留它们是因为：① 症状描述高度相似，值得记录；② 里面的排除手段（呈现模式、compositeAlpha）以后再遇到同类现象能省时间。

### 排查中被证伪的"环境侧规避"（记录以免重复尝试）

| 尝试 | 结果 |
|---|---|
| 强制 `VK_PRESENT_MODE_IMMEDIATE_KHR`（本卡只支持 IMMEDIATE / FIFO / FIFO_RELAXED，**无 MAILBOX**） | 症状**完全不变** → 与呈现模式无关 |
| `VK_SUBOPTIMAL_KHR` → 重建交换链 | 代码**本来就这么写的**（`drawFrame` 的 present 分支已含 SUBOPTIMAL），不是缺失项 |
| NVIDIA 控制面板 "Vulkan/OpenGL present method" 改 native | AMD 卡无此开关 |
| HAGS / 驱动版本 / 关 overlay | 未尝试（根因已定位，不必再试） |

### 教训

1. **"窗口模式才出现"不一定是呈现层**：两个 in-flight 帧共用一份每帧更新的资源，
   在全屏（帧慢）时不撞、在窗口（帧快）时撞 —— 表现为"和窗口模式强相关"，误导性极强。
2. **每帧更新的 GPU 资源必须按 frame-in-flight 分份**（UBO / 描述符 / 任何 host 可见缓冲），
   否则就是这种"偶发一帧"的错乱。检查清单：UBO、staging buffer、动态描述符更新。
3. 排查顺序里应**更早**检查"每帧数据是怎么送到 GPU 的" —— 这次它排在第 8 位，
   而前面 7 项（网格/深度/事件/同步/信号量/extent/裁剪）本来都有硬数据可快速否定。

## 十一、物理刚体"冻住"（2026-11-27，两条，均已修）

| # | 类型 | 位置 | 现象 | 根因 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B139 | 跳过条件用错语义 | `PhysicsWorld::stepJolt` | 刚体静置后完全不动，只有玩家碰到才动一下；被推下平台会**悬在空中** | 跳过条件是 `GetNumActiveBodies(...)==0`。`active` = **醒着**的刚体，睡眠后为 0 → `Update` 永不调用 → 重力不再积分。玩家（CharacterVirtual）接触会唤醒它，所以"碰一下动一下" | 🔴 | ✅ 已修（改为"有没有动态刚体"）`9d0b721` |
| B140 | 睡眠体不被唤醒 | `PhysicsWorld::onPackContraption`（`mAllowSleeping`）| B139 修完后仍然"冻住" | Jolt 让静止刚体睡眠后，**即使 Update 也不会积分**；只有接触或显式 `ActivateBody` 才醒。于是"挖掉它脚下的方块 / 改地形"都不会让它动 | 🔴 | ✅ 已修（原型阶段 `mAllowSleeping=false`，注释写明将来改回"按需唤醒"的时机）`b2dbb01` |

> **教训**：两条都属于"基线指标看不出来"的缺陷 —— 9 项基线里 tps/方块数/预热全正常，
> 但物理行为是错的。物理这条链缺**行为级自检**（例：给刚体初速度 + 重力，跑 N 步断言落点），
> 只有"能否跑起来"的检查是不够的。
## 十二、刚体原点取的是"包围盒角"而不是中心（2026-11-27，新发现，未修）

| # | 类型 | 位置 | 现象 | 根因 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B141 | 坐标约定不一致 | `PhysicsWorld::onPackContraption` + `VulkanMesh::buildBodyVertices` | 纯平移时看不出问题；一旦刚体**旋转**，它会绕"包围盒角"转而不是绕自己中心转（07 转子/多体时必然暴露）| 打包时 `origin = (min+max)/2`（整数，取的是格点），而形状/网格又把方块中心放在 `局部 pos + 0.5` —— 两者叠加等价于"原点 = 包围盒的最小角"。渲染与物理**两边一致**，所以平移正常、只有旋转才错 | 🟡 | ✅ **已修**（`shared/block/StructureFrame.hpp`：原点改成真·几何中心；物理形状偏移 / 渲染网格顶点 / 解包回写 三处共用同一份数学；用例④「中心约定」守回归） |

> **实际修法（2026-11-27）**：不动 `BlockPos`（局部坐标仍是整数，map/邻居逻辑不受影响），
> 而是把"几何中心 − 整数基准"这个 bias 抽成 **一份共享数学** `shared/block/StructureFrame.hpp`：
> `structureOriginAxis / structureCenterAxis / structureBias / localBlockCenter`。
> 物理形状偏移、渲染网格顶点、解包回写三处都调它 —— 两边不可能各算一套（那正是 B141 的成因）。
> 自检用例④专门验证"位姿 = 方块中心的平均"（偶数尺寸 bias=1.0、奇数 0.5）。
## 十三、F10/F11 刚体交互导致的整引擎冻结（2026-11-27，**已修**）

| # | 类型 | 位置 | 现象 | 现状 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B142 | 冻结 | `BodyMeshStore::upload` 立刻销毁旧缓冲（`onInteractStructure` → `rebuildContraptionShape` 触发的网格重建） | 按 F10 后方块抖动一下，随后**整个引擎冻结**：画面定格、视角也不能动；进程仍在、19 线程全 Wait、CPU≈0；`logs/engine.log` 最后一行被截断在时间戳处 | ✅ **已修**（`f53a030`）：刚体网格旧缓冲改延迟销毁（P14 式按帧槽墓地）；`vkWaitForFences` 加 5s 超时把"哑死"变成可诊断错误 | 🔴 | ✅ 独立看门狗定位：渲染帧停在 4168、主/业务/物理心跳继续 → 渲染线程卡死 |

**已排查/已排除**：
- ✅ 已排除：日志异步队列 `block` 策略（当天改成 `overrun_oldest` + 关控制台快速编辑，见 `e597e00`）—— 用户跑的是修复后的二进制，仍然冻结。
- ✅ 已排除：短时间重复交互（8 轮 F9/F10/F11 压力跑未复现）。
- ❓ 未验证的可疑点（下次重做时按顺序查）：
  1. `Jolt BodyInterface::SetShape` + `SetPositionAndRotation` 在**紧邻 CharacterVirtual 接触**时的行为；
  2. `SetShape(updateMassProperties=true)` 重算质心后与"帧偏置补偿"的组合是否让某处进入不一致状态；
  3. 渲染侧 `BodyStructureEvent(destroy=false)` → `uploadBodyMesh` 在**同一结构短时间内连续重建**时的 buffer 生命周期；
  4. 冻结时日志被截断 → 可能确实是某一环阻塞了 sink（需用独立文件看门狗，而不是日志，来取证）。

**下次重做时的前置**：先加"独立看门狗"（自开文件、500ms 写心跳：主/业务/物理/渲染各一个计数 + 阶段标记），
复现后看哪个心跳先停、停在哪个阶段 —— 这次就是没先做这一步，导致只能靠猜。
## 十四、刚体上的放置/破坏"时灵时不灵"（2026-11-27，已修）

| # | 类型 | 位置 | 现象 | 现状 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B143 | 坐标边界 | `PhysicsWorld::castStructureRay` 的"命中点 → 方块格" | 对着刚体点左/右键，有时生效、有时毫无反应；日志是"未生效（射线未命中刚体 / 目标格不可用）" | ✅ 已修（`81090ca`） | 🟡 | ✅ 命中点正好落在格子边界时 `floor(局部 + 偏置)` 会取到**体外空格** |

**根因**：命中点常在格子的**表面**上。例如打在 +z 面上，局部 z = 1.5（方块 p=1 的面），
而 `floor(1.5 + bias(0.5)) = 2` —— 越界。`findBlock` 找不到该格 → 判为未命中。

**修法**：命中点先沿射线方向**回退 1e-3** 再取格（射线由外向内，回退必然进入被击中的那一格）；
回退后仍不在体内（掠射/角点/浮点误差）时，在 6 邻居里选距离最近的**实心**格兜底。

**教训**：探针（ProbeStructure）与执行（InteractStructure）必须共用同一份推导函数 ——
本次就是先发现"探针命中、执行不命中"，才顺藤摸到这条边界 bug。
## 十五、每次在刚体上放/拆方块，刚体都会"抖一下"（2026-11-27，已修）

| # | 类型 | 位置 | 现象 | 现状 | 严重度 | 核实 |
|---|---|---|---|---|---|---|
| B144 | 坐标系 / 物理 | `structureBias()` 用**包围盒中心**；`rebuildContraptionShape` 又补偿了一次位置 | 每次在刚体上放或拆一块，刚体都会明显位移一下（几何整体跳半格左右） | ✅ 已修 | 🟡 | ✅ Jolt `Body.cpp:117 UpdateCenterOfMassInternal`：`mPosition += R*(新COM−旧COM)` |

**根因**：Jolt 换形状时会**自己挪一次体**（按 COM 变化，保持几何不动）。
而我们的帧偏置取的是"包围盒中心"，不是质心 —— 结构一旦不规则（编辑后很常见），
形状的 COM 就不为 0，于是 Jolt 挪一次、我们再按 (新偏置 − 旧偏置) 挪一次
→ 净位移 = 新 COM（最多半格）→ 玩家看到"抖一下"。

**修法**：帧偏置改成**质心**（`structureBias` = 平均(方块中心)）。
于是每个子形状偏移的均值恒为 0 ⇒ 形状 COM 恒为 0 ⇒ Jolt 永不自行挪体，
我们只补偿 (新偏置 − 旧偏置)，几何正好原地不动。
规整矩形（3×3×3 等）质心与包围盒中心的值相同（都是 0.5），所以既有用例期望值不变。
## 汇总

| 项 | 数量 |
|---|---|
| 核实条目 | **144 条**（B1–B144；B135/B136 于 2026-11-26 销案；B139/B140 于 2026-11-27 修；B141 新发现未修） |
| **当前未结缺陷** | **0 条**（B141、B142 已于 2026-11-27 修）。其余为 ✅已修 / ⏸已评估不改 / ❌文档误记。B98、B137 属「未实现的新功能」；B94/B103/B116 等落在近期会重做的代码里（Jolt 多刚体 / 区块重建+LOD / AO） |
| 其中 🔴 | **15 条**（含已销案的 B135/B136） |
| 文档误记（❌ 不实） | **8 条**（TimeManager ×2、ChunkData ×1、PlayerBodyState ×1、ChunkPos ×1、Protocol ×3） |
| 已被重构修复（🔧） | **3 条**（BodyPose 缺旋转、`queue*` 命名、"事件流语义"） |
| 归类为性能（→ `性能问题记录.md`） | 8 条（P1–P8）→ 现共 14 条（P1–P14） |
| 归类为渲染端注意项（规则 2/5 豁免） | 3 条（CameraState→Vulkan） |
| **本次已修（✅）** | **2 条**（B137 全量重建入口缺失——改名+记录；**B138 窗口模式地形局部透明——根因是共享 UBO 竞态**，见第十节） |

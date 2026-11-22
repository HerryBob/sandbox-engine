# bug 记录

> 只记录不修。严重度：🔴 会崩 / 🟡 行为错（特定条件触发）/ 🟢 隐患或诊断缺失
> 来源：通读记录（U12）+ 文件分析（BusinessWorld / PhysicsBridge / RenderBridge）。
> 性能类问题不在此表，见 `性能问题记录.md`。

| # | 类型 | 位置（文件:行 + 函数） | 现象 | 根因 | 影响 | 严重度 | 状态 |
|---|---|---|---|---|---|---|---|
| **B1** | 生命周期顺序 | `src/modhost/loader/ModLoader.cpp:270`<br>`ModLoader::unloadAll` | `destroy(self)` 早于 `eraseByOwner(name)`（270 → 281） | 顺序写成"先销毁模组，再清注册" | 两步之间注册表内仍留指向已销毁对象的指针：`ModEntityDef::animUser` 悬垂、噪声 provider 函数指针仍可被调到。正确顺序应为 `onUnload → eraseByOwner → destroy → FreeLibrary` | 🟡 | 未修 |
| **B2** | 逻辑漏判 | `src/modhost/sorter/ModSorter.cpp:76`<br>`ModSorter::sort` | 跨批依赖"环上模组"的模组不会被拒 | 第 4 步级联在跑到第 5 步环检测之前，环被标拒后不再级联 | 该类模组进入 `order` 却缺依赖；`ModLoader` 的 `failedIds` 兜不住（被拒模组不进 `order`）。修法：级联挪到环检测之后 | 🟡 | 未修 |
| **B3** | 缺校验 | `src/modhost/block/BlockApi.cpp:42`<br>`registerBlockImpl` | `mass` / `hardness` / `friction` 不做 `isfinite` 与范围校验 | 只校验了空指针、冻结、id 上限 | 模组可传入 `NaN` / 负数 / 极端值，接 Jolt 后会污染质量与摩擦属性 | 🟡 | 未修 |
| **B4** | 缺校验 | `src/modhost/block/BlockApi.cpp:48`<br>`registerBlockImpl` | 同名方块不检测：两个模组都注册 `"stone"` 都会成功 | 只校验 `name` 非空，不校验唯一 | `findBlockIdByName()` 返回哈希桶顺序里第一个命中，结果不确定 | 🟢 | 未修 |
| **B5** | 隐式依赖 | `src/modhost/loader/ModLoader.cpp:133`<br>`ModLoader::loadAll` | `builtin` 组内按 `manifest.id` 字典序排序，无显式优先级 | 只有 `phase` 两级，组内顺序借用 id 排序 | 方块 id 1..6 的稳定性依赖 `"builtin_blocks"` 恰好字典序靠前；新增 `aaa_*` 的 builtin 模组会让方块 id 整体 +N（旧存档错位） | 🟡 | 未修 |
| **B6** | 诊断缺失 | `src/modhost/shared/LogImpl.cpp:15`<br>`logImpl` | 模组日志 tag 固定 `"Mod"`，不带模组名 | 未使用 `g_host.currentMod` | 多个模组的日志混在一起无法归属 | 🟢 | 未修 |
| **B7** | 回滚不完整 | `src/modhost/loader/ModLoader.cpp:225`<br>`ModLoader::loadAll`（`onLoad` 失败分支） | `onLoad` 返回 false 时只回滚噪声注册，不清实体类型 | 失败分支只调了 `NoiseRegistry::eraseByOwner` | 模组先 `registerEntity` 成功、后 `onLoad` 失败 → 实体类型残留在 `g_entityTypeRegistry`，其 `onAnimate` 指向随后被 `FreeLibrary` 的 DLL → 悬垂函数指针 | 🟡 | 未修 |
| **B8** | 异常无防护 | `src/modhost/loader/ModLoader.cpp:268`<br>`unloadAll`（同样适用于 `onLoad`） | `vtbl->onUnload/onLoad` 直接调用，引擎侧无 `try/catch` | 改纯 C 接口时移除了 `ModBridge<T>` 的 `try/catch` | 用 C++ 写的模组若在 `onUnload`/`onLoad` 里抛异常，会穿过 C 边界（UB）。`mod_c_api.h` 只写了"不得抛异常"的约定 | 🟢 | 未修 |
| **B9** | 覆盖丢事件 | `src/business/PhysicsBridge.cpp:17`<br>`pushPlaceCommand` / `pushRemoveCommand` | 放置/破坏用**单槽位**（`wantPlace_` / `wantRemove_`）而不是队列：同一 tick 内的第二条指令覆盖第一条 | 用"置位"实现事件语义 | 世界与物理不同步 —— 方块看得见但穿得过去；批量操作（DDA 连续破坏）必现 | 🔴 | 未修 |
| **B10** | 失败不致命 | `src/main.cpp`<br>`warmupWindow()` 30s 超时分支 | 预热超时只打印 `[未灌满：30s 超时]` 就继续启动 | 把预热当"尽力而为"，未当致命失败 | 世界可能只有部分列；渲染端可能一个 chunk 都没有，且后续表现难以归因 | 🔴 | 未修 |
| **B11** | 负下标 | `src/worldgen/generator/ColumnGenerator.cpp:138`<br>`generateColumn` | `out.chunks[y >> 4]` 把绝对 y 直接当层号下标；`kGenMinY < 0` 时 y 为负 → 负下标（内存破坏） | 层号用"绝对 y >> 4"，而 `kGenMinY` 被当作可配常量 | 一旦把 `kGenMinY` 改成负数即越界写。修法：`static_assert(kGenMinY >= 0)`，或改 `(y - kGenMinY) >> 4` | 🔴 | 未修 |
| **B12** | 覆盖丢状态 | `src/business/BusinessWorld.cpp:56`<br>`receiveResultsFromPhysics` | 位姿用 `std::optional<BodyPose>` 只能装一个：一个 tick 内多刚体发位置时后到覆盖先到 | 用单槽承载多实体状态 | 现在 1 演示刚体 + 1 玩家体，两者一直在互相覆盖："看起来没事"但多刚体必错 | 🟡 | 未修 |
| **B13** | 死生产 | `src/business/PhysicsBridge.cpp:112` | 碰撞事件仍在生产，但没有任何消费方 | 求解器下线后消费方被删、生产方没删 | 每次碰撞都白进一次队列（且 B12 的单槽位问题同样适用于它） | 🟡 | 未修 |
| **B14** | 队列满恶化 | `src/business/RenderBridge.cpp:15`<br>`flushChangesToRender` | push 失败时**不清空**脏集合 → 下 tick 脏得更多 → 每次失败都拷更大一份（雪崩） | "失败就下 tick 重试"没考虑重试成本递增 | 渲染持续落后时，业务线程的拷贝量单调增长 | 🟡 | 未修 |
| **B15** | 单 tick 尖峰 | `src/worldgen/streaming/ChunkStreamer.cpp`<br>`receiveResults` | 属于已淘汰列的结果 `continue` 丢弃但不计入 `applied` → 队列积压时一次 drain 扫空全部 | 循环条件只有 `applied < maxApply` | 玩家瞬移/快速移动后单 tick 长耗时（掉帧尖峰） | 🟡 | 未修 |
| **B16** | 闪洞 | `src/worldgen/streaming/ChunkStreamer.cpp`<br>`applyPendingEvictions` | `pendingEvict_` 同 tick 结算 → 玩家在窗口边界来回走时该列空 2~3 tick（~80–120ms 闪洞） | 结算前没复查"该列是否又回到窗口内" | 边界来回走时可见地形闪烁 | 🟡 | 未修 |
| **B17** | 线程归属无保护 | `src/worldgen/streaming/ChunkStreamer.hpp` | 业务线程（`tick`/`receiveResults`）与生成线程（`workerLoop`）共用一个对象，归属只靠注释 | 线程跟着数据走，但没做断言 | 在错误线程调用即数据竞争，且无任何提示 | 🟡 | 未修 |
| **B18** | 反向依赖 | `src/worldgen/streaming/ChunkStreamer.cpp`<br>`receiveResults(World&)` | `worldgen` 反向依赖 `business::World`（全项目唯一反向依赖），且函数身兼"展开/调用/过滤/统计"四职 | 为把"唯一 World 写者"集中在一处 | 生成器搬 DLL 时这条路走不通（规则 7 的边界） | 🟡 | 未修 |
| **B19** | 隐式三环契约 | `src/business/World.cpp:226` + `VulkanContext::rebuildChunkMeshes` | "卸载 → 重建时取不到方块 → 删 mesh"是三环隐式契约，任一环被"优化"就留幽灵网格 | 无类型/断言保护，只有注释 | 有人把无顶点分支改成 `continue` 即出现幽灵网格 | 🟡 | 未修 |
| **B20** | 封装泄漏 | `src/business/BusinessWorld.hpp`<br>`world()` | 返回非 const `World&`，与"运行期外部不得持有引用"的注释矛盾 | 装配期与运行期共用同一入口 | 调用方可绕过 `BusinessWorld` 的调度（直接 `placeBlock` 绕过统计与排队、直接 `clearDirtyChunks` 绕过 flush 时序） | 🟡 | 未修 |
| **B21** | 容量不足 | `src/business/RenderBridge.hpp:44` + `BusinessWorld.cpp:64` | 结构事件"不能丢"但队列只有 16 深；同 tick 打包 20 个刚体即失败，且调用方忽略了返回值 | 容量按"1~2 个刚体"设定 | 超出容量时静默丢结构事件 → 渲染端缺网格 | 🟢 | 未修 |
| **B22** | 能力缺失 | `src/business/BusinessWorld.hpp` | 只转发了 `findBlock`（两态），没转发 `lookupBlock`（三态） | 未同步补转发 | 外部想区分"未加载 vs 空气"只能走 `world()`（又与 B20 冲突） | 🟢 | 未修 |
| **B23** | 语义易混 | `src/business/BusinessWorld.hpp` | `syncAll()`（一次性，失败返回 false）与 `requestPhysicsResync()`（置标志，每 tick 自动重试）看起来一样 | 两个名字都像"请求同步" | 调用方容易选错，导致"不重试"或"重复重试" | 🟢 | 未修 |
| **B24** | 约束未注明 | `src/business/BusinessWorld.hpp`<br>`placeBlock` / `removeBlock` | 实现里往 SPSC 队列塞东西（单生产者），但头文件没写"只允许业务线程调用" | 依赖读者的全局理解 | 从其它线程调用破坏单生产者约定（UB） | 🟢 | 未修 |
| **B25** | 非原子标志 | `src/business/BusinessWorld.hpp`<br>`resyncPending_` / `firstTick_` | 两个 bool 不是原子，全靠"业务线程独占"的注释 | 为省原子 | 若将来从 main 线程调 `requestPhysicsResync()` 即与业务线程竞争 | 🟢 | 未修 |
| **B26** | 封装泄漏 | `PhysicsBridge` / `RenderBridge` 的队列访问器 | 返回公开可变引用，谁都能拿队列绕过桥 | 为装配层方便 | 任意代码可绕过桥的统计与语义约定（如绕过"失败不清脏"） | 🟢 | 未修 |
| **B27** | 死字段 | `src/business/RenderBridge.cpp:13` | `ev.tick` 只写不读（全项目无读取） | 预留调试字段 | 读者会以为它被用于去重/排序 | 🟢 | 未修 |
| **B28** | 背压不可见 | `RenderBridge` 整体 | 业务不知道渲染消费到哪；`Renderer::changeEventsReceived()` 只在三轨停止后用于断言 | 没有运行期监控 | 积压只能靠症状发现 | 🟢 | 未修 |
| **B29** | 死代码 | `src/business/BusinessWorld.hpp`<br>`syncAll()` | 声明并实现，但全项目从未调用 | 预留接口 | 与 B23 叠加，读者难以判断该用哪个 | 🟢 | 未修 |
| **B30** | 注释失真 | `src/worldgen/streaming/ChunkStreamer.hpp:36` | 注释写"一次锁写 64KB"，实际一列调 4 次 `applyGeneratedChunk`（4 次锁），每 tick 最多 16 次 | 把"一列"和"一次锁"混为一谈 | 基于该注释做性能判断会错 | 🟢 | 未修 |
| **B31** | 约束未注明 | `ChunkStreamer::stats()` | 聚合字段（`appliedBlocks_` / `minSurfaceY_`）只能在业务线程读，无编译期防护 | 为省原子用普通成员 | 从 main 线程读 `stats()` 即数据竞争 | 🟢 | 未修 |
| **B32** | 约束未注明 | `ChunkStreamer.hpp:34` | `kMaxApplyPerTick=4` 与 `kResultCapacity=64` 的反压关系没写（业务卡 16 tick 以上会反压生成线程） | 只写了各自含义 | 调整任一时容易忽略反压 | 🟢 | 未修 |

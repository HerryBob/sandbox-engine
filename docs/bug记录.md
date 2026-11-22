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

## 四、待核实（下一批）

| 文档 | 条数（🔴/🟡/🟢） |
|---|---|
| `shared/protocol/Protocol_hpp.md` | 3 / 4 / 5 |
| `registry/BLockInfo_hpp.md` | 1 / 2 / 3 |
| `registry/BlockRegistry_hpp.md` | 1 / 5 / 3 |
| `registry/Registry_hpp.md` | 1 / 1 / 4 |
| `business/World_hpp_cpp.md` | 1 / ~15 / ~10 |
| `business/BusinessWorld_hpp_cpp.md` | 1 / ~14 / ~4 |
| `business/PhysicsBridge_hpp_cpp.md` | 1 / 5 / 2 |
| `business/RenderBridge_hpp_cpp.md` | 0 / 3 / 4 |
| `business/base/BlockActions_hpp_cpp.md` | 0 / 6 / 3 |
| `worldgen/streaming/ChunkStreamer_hpp_cpp.md` | 2 / 5 / 3（与 U12 重复，需对一遍） |

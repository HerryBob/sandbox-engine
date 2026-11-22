# bug 记录

> 只记录不修。严重度：🔴 会崩 / 🟡 行为错（特定条件触发）/ 🟢 隐患或诊断缺失

| # | 类型 | 位置（文件:行 + 函数） | 现象 | 根因 | 影响 | 严重度 | 状态 |
|---|---|---|---|---|---|---|---|
| **B1** | 生命周期顺序 | `src/modhost/loader/ModLoader.cpp:270`<br>`ModLoader::unloadAll` | `destroy(self)` 早于 `eraseByOwner(name)`（270 → 281） | 顺序写成"先销毁模组，再清注册" | 两步之间注册表内仍留指向已销毁对象的指针：`ModEntityDef::animUser` 悬垂、噪声 provider 函数指针仍可被调到。正确顺序应为 `onUnload → eraseByOwner → destroy → FreeLibrary` | 🟡 | 未修 |
| **B2** | 逻辑漏判 | `src/modhost/sorter/ModSorter.cpp:76`<br>`ModSorter::sort` | 跨批依赖"环上模组"的模组不会被拒 | 第 4 步级联在跑到第 5 步环检测之前，环被标拒后不再级联 | 该类模组进入 `order` 却缺依赖；`ModLoader` 的 `failedIds` 兜不住（被拒模组不进 `order`）。修法：级联挪到环检测之后 | 🟡 | 未修 |
| **B3** | 缺校验 | `src/modhost/block/BlockApi.cpp:42`<br>`registerBlockImpl` | `mass` / `hardness` / `friction` 不做 `isfinite` 与范围校验 | 只校验了空指针、冻结、id 上限 | 模组可传入 `NaN` / 负数 / 极端值，接 Jolt 后会污染质量与摩擦属性 | 🟡 | 未修 |
| **B4** | 缺校验 | `src/modhost/block/BlockApi.cpp:48`<br>`registerBlockImpl` | 同名方块不检测：两个模组都注册 `"stone"` 都会成功 | 只校验 `name` 非空，不校验唯一 | `findBlockIdByName()` 返回哈希桶顺序里第一个命中，结果不确定 | 🟢 | 未修 |
| **B5** | 隐式依赖 | `src/modhost/loader/ModLoader.cpp:133`<br>`ModLoader::loadAll` | `builtin` 组内按 `manifest.id` 字典序排序，无显式优先级 | 只有 `phase` 两级，组内顺序借用 id 排序 | 方块 id 1..6 的稳定性依赖 `"builtin_blocks"` 恰好字典序靠前；新增 `aaa_*` 的 builtin 模组会让方块 id 整体 +N（旧存档错位） | 🟡 | 未修 |
| **B6** | 诊断缺失 | `src/modhost/shared/LogImpl.cpp:15`<br>`logImpl` | 模组日志 tag 固定 `"Mod"`，不带模组名 | 未使用 `g_host.currentMod` | 多个模组的日志混在一起无法归属 | 🟢 | 未修 |
| **B7** | 回滚不完整 | `src/modhost/loader/ModLoader.cpp:225`<br>`ModLoader::loadAll`（`onLoad` 失败分支） | `onLoad` 返回 false 时只回滚噪声注册，不清实体类型 | 失败分支只调了 `NoiseRegistry::eraseByOwner` | 模组先 `registerEntity` 成功、后 `onLoad` 失败 → 实体类型残留在 `g_entityTypeRegistry`，其 `onAnimate` 指向随后被 `FreeLibrary` 的 DLL → 悬垂函数指针 | 🟡 | 未修 |
| **B8** | 异常无防护 | `src/modhost/loader/ModLoader.cpp:268`<br>`unloadAll`（同样适用于 `onLoad`） | `vtbl->onUnload/onLoad` 直接调用，引擎侧无 `try/catch` | 改纯 C 接口时移除了 `ModBridge<T>` 的 `try/catch`，改为直接走 C 函数指针 | 用 C++ 写的模组若在 `onUnload`/`onLoad` 里抛异常，会穿过 C 边界（UB）。`mod_c_api.h` 只写了"不得抛异常"的约定，无强制手段 | 🟢 | 未修 |

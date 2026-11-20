# 架构决策 · 模组 API 演进策略

> 状态：**定案，未实施**（2026-11-19）
> 起因：通读 U10 噪声时审查 `modapi/` 现状，讨论"定期重构 + 期间按社区要求追加"的可行性。
> 关联：[模组架构决策.md](模组架构决策.md)、[架构决策-模组化分层.md](架构决策-模组化分层.md)、[模组自由度与责任边界.md](模组自由度与责任边界.md)

---

## 一、现状（v5）问题清单

### 🔴 致命（会挡住"追加式演进"或跨编译器）

| # | 问题 | 现象 | 改法 | 成本 |
|---|---|---|---|---|
| **1** | **版本校验用 `!=`** | 模组里 `if (api->x.version != kEngineApiVersion) return false;`，而 `kEngineApiVersion` 是**模组编译时烧进去的**。引擎追加能力升到 v5 → 老模组（编译时 v4）读到 5 → `5 != 4` → **被拒绝加载**。**"末尾追加"的全部意义就是不重编也能跑，这条校验把它废了。** | 拆成双版本号（见 §二） | 一行 |
| **2** | **`IMod` 是 C++ 虚接口** | 依赖"同编译器、同 vtable 布局"（`IMod.hpp:23` 注释自己承认）。而同库的 `NoiseProviderDef` 用 C 函数指针，注释明说"不需要共享任何 ABI"——**两种策略并存** | 入口改 C 函数表：`struct ModHandle { void* self; const ModVtbl* vtbl; }`，与 `NoiseHandle` 同构 | 中（改 IMod + 3 个模组） |
| **3** | **无崩溃隔离** | 模组 `onLoad`/回调里一个野指针 → 整个进程死 | 每次跨边界调用包 SEH（`__try/__except`），失败则禁用该模组 | 中 |

### 🟡 结构（会导致"每加一个能力就漏一次"）

| # | 问题 | 现象 | 改法 | 成本 |
|---|---|---|---|---|
| **4** | **`ApiBase` 在每个子 API 各复制一份** | `BlockApi`/`NoiseApi`/`WorldApi`/`EntityApi` 各带一份 `version/host/log` → 4 份冗余；语义奇怪（模组写 `api->noise.log(...)`，但 log 不属于 noise）；新加能力又要复制一遍 | `ModApi` 顶层放一份 `ApiBase base;`，子 API 只放函数指针 | 小 |
| **5** | **回滚每个能力各写一遍** | `ModLoader.cpp:216` 只调了 `NoiseRegistry::eraseByOwner`，方块/实体没管 → **R11**（实体类型残留 + 悬垂 `onAnimate`）。根因是 `eraseByOwner` 各注册表各自实现，`ModLoader` 得挨个记得调 | 抽 `IContentRegistry{ virtual eraseByOwner }`，所有注册表实现它，ModLoader 遍历列表统一回滚 | 小 |
| **6** | **"字符串/数组只在调用期间有效"无法验证** | `NoiseParamC::key/str` 指向临时 `std::string`，要求 provider 自己拷贝——零检查。同类问题会出现在**每个**"传数组"的 API 上 | 调试构建用哨兵法：`make()` 后用 memset 污染参数内存，provider 若存了指针，下次读取即脏数据 → 测试可抓 | 中 |

### 🟢 表达力（决定"模组能做多深"）

| # | 缺什么 | 后果 |
|---|---|---|
| **7** | **运行期钩子**（onTick / onBlockPlaced / onEntitySpawn） | 模组只能"注册内容"，不能"每 tick 做事"→ **方块行为运行时做不了**（自由度最大瓶颈） |
| **8** | 能力查询（`hasCapability`） | 模组只能手工判 `registerXxx != nullptr` |
| **9** | 模组间通信（事件总线） | 两个模组互相不知道对方存在 |
| **10** | 参数 schema 校验 | 噪声参数名拼错 → **静默用默认值**，无任何警告 |

---

## 二、演进策略：扩张期 + 周期性压实

### 决策 1：拆成两个版本号（**P0**）

```cpp
// 破坏性变更才 +1：删函数、合并函数、重排结构体
inline constexpr std::uint32_t kAbiVersion   = 1;
// 追加能力就 +1：新加子结构、新加函数（末尾追加，ABI 不变）
inline constexpr std::uint32_t kFeatureLevel = 5;
```

模组侧两条校验，语义完全不同：

```cpp
// ① 硬校验：ABI 必须精确相等（重构期过后，老模组必须重编）
if (api->base.abiVersion != modapi::kAbiVersion) return false;
// ② 软校验：能力必须够用（扩张期，老模组照常跑）
if (api->base.featureLevel < kFeatureLevelINeed) return false;
```

| 场景 | 单一 version（现状） | 双版本号 |
|---|---|---|
| 引擎追加 `entity` | `5 != 4` → **误拒老模组** ❌ | abi 不变 → 照常跑 ✅ |
| 引擎删掉旧函数 | `6 != 4` → 拒绝（碰巧对） | abi 2≠1 → 拒绝（语义明确）✅ |

### 决策 2：三阶段节奏

| 阶段 | 时长 | 做什么 | 改哪个号 |
|---|---|---|---|
| **扩张期** | 3–6 个月（90% 时间） | 按社区要求追加 API，只加不删 | `kFeatureLevel +1` |
| **冻结期** | 2 周 | 停止加新 API，发布「下个周期删除清单」 | —— |
| **重构期** | 1–2 周 | 删除重复、合并重合、重排结构 | **`kAbiVersion +1`** |

**废弃期规则**：合并/删除前必须先让新老函数**并存一个周期**（`[[deprecated]]` 标注），下个周期再删。否则社区今天照文档写的模组，明天就被删函数。

### 决策 4：入口改 C 接口，但**两层共存**（2026-11-19 定案）

**决定**：模组入口改为 C 接口（函数表 + POD），因为形式开放性差距太大：

| 维度 | C 接口 | C++ 虚接口（现状 `IMod`） |
|---|---|---|
| 语言 | 任何（Rust/Python/脚本/WASM） | 只能 C++ |
| 编译器 | 任何 | **必须同版本同编译器** |
| 热重载 | 可 | 难（vptr） |
| 可验证性 | 可逐个检查函数指针 | vtable 本身无法验证 |

**做法：不删 `IMod`，另加一个 C 入口 —— 引擎先找 `CreateModC`，找不到再退回 `CreateMod`。**

```c
// 通用 C 入口（任何语言、任何编译器）
extern "C" int CreateModC(const modapi::ModApi* api, ModHandleC* out);
// 官方 SDK 入口（C++ 虚接口，写起来舒服）
extern "C" modapi::IMod* CreateMod(const modapi::ModApi* api);

struct ModVtbl {
    const char* (*name)(void* self);
    bool (*onLoad)(void* self, const modapi::ModApi* api);
    void (*onUnload)(void* self);
};
struct ModHandleC { void* self; const ModVtbl* vtbl; };
```

**业界参照**：Godot 的 GDExtension 是唯一大规模用 C++ 虚接口跨 DLL 的例子，代价是"必须用官方指定编译器"——这就是 C++ 入口的门槛。MC 生态繁荣的隐藏原因是 Java 有标准 ABI。

**时机**：定案，**暂不实施**（1.0 之前模组作者只有自己，改不改都行；等有外部模组作者再动）。

---

### 决策 3：重复判断要在「加」的时候做

维护一张**能力职责表**，新增前先查表：

| 能力 | 唯一入口 | 禁止出现 |
|---|---|---|
| 读方块 | `world.getBlock(pos)` | `getBlockXYZ`、`getBlockAt`、`readBlock` |
| 写方块 | `world.setBlock(pos, type)` | `placeBlock`、`writeBlock` |

---

## 三、改动优先级

| 优先级 | 改什么 | 成本 | 收益 |
|---|---|---|---|
| **P0** | 双版本号（`kAbiVersion` + `kFeatureLevel`） | 一行 | 让追加式演进真正成立 |
| **P0** | 统一回滚接口 `IContentRegistry` | 小 | 根治 R11 一类问题 |
| **P1** | `ApiBase` 提到 `ModApi` 顶层 | 小 | 结构干净、新能力不膨胀 |
| **P1** | POD 断言（`static_assert(is_standard_layout)`）+ 结构体 `size` 字段 | 小 | 布局错位可自检 |
| **P2** | `IMod` 改 C 函数表 | 中 | 跨编译器稳定 |
| **P2** | 崩溃隔离（SEH） | 中 | 坏模组不毁全进程 |
| **P3** | 运行期钩子 | 大 | 自由度跃升（依赖方块行为运行时） |

---

## 四、1.0 前后的分界（**最重要的判断**）

| 阶段 | 建议 |
|---|---|
| **1.0 之前** | **自由重构，不管兼容**。模组都是自己的（`blocks`/`noise`/`player_entity`），改起来 10 分钟。不要提前引入冻结期/删除清单，那只是给自己加流程负担。 |
| **1.0 发布时** | 定下 `kAbiVersion = 1`，开始遵守纪律 |
| **1.x** | 扩张期为主，**一年最多一次重构** |

**每次重构的社区成本**：模组作者每个 0.5–2 天；**玩家侧老模组全部失效直到作者更新**——所以频率不能高（一年 1 次是上限）。

---

## 五、不变式

| # | 不变式 | 违反了会怎样 | 靠什么守 |
|---|---|---|---|
| 1 | `ModApi` 只在**末尾追加**子结构 | 老模组按旧偏移读 → 读到垃圾函数指针 → 崩 | `kFeatureLevel` 校验 + 评审 |
| 2 | 删/合并/重排必须 `kAbiVersion +1` | 老模组调到错误的槽 → 崩（不是拒绝加载） | `!=` 硬校验 |
| 3 | 字符串一律拷成副本，绝不存 DLL 指针 | 卸载后悬垂 | `std::string` 拷贝（现状已做） |
| 4 | 谁分配谁释放（create/destroy 对称） | 跨 CRT 释放 → 崩 | `NoiseHandle` 式三件套 |
| 5 | 回滚必须按 owner 清**所有**注册表 | 残留 + 悬垂回调（R11） | 待做：`IContentRegistry` |

---

## 六、待办

- [ ] P0：`kAbiVersion` / `kFeatureLevel` 拆分 + 所有模组校验改为双校验
- [ ] P0：`IContentRegistry` 抽接口，`ModLoader` 统一回滚（顺手修 R11）
- [ ] P1：`ApiBase` 提到 `ModApi` 顶层
- [ ] P1：`static_assert` POD + `ModApi` 加 `size` 字段
- [ ] 文档：能力职责表（新增 API 前查表）

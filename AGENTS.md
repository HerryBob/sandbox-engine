# 项目指令（AGENTS.md）

本文件是 DSH 的项目级指令，`.git` 所在目录即项目根，加载范围为根目录到会话工作目录。

---

## 0. 操作范围（最高优先级）

**只允许在 `E:\MinecraftGame2` 内做无限制的读写、构建、提交。**

| 位置 | 允许 |
|---|---|
| `E:\MinecraftGame2\**` | 读、写、改、删、构建、提交、推送 |
| 该目录之外的一切路径 | **只读**。禁止创建、修改、删除任何文件 |
| 系统设置 / 全局配置 / 注册表 / 环境变量 | **禁止修改** |

补充约束：

- **临时文件也必须留在工作区内**（用 `build\` 下的子目录），不要在 `%TEMP%` 或用户目录写东西。
- 需要读工作区外的文件（例如 DSH 自身文档、第三方参考）时只读，读完不要留下副本。
- 用户明确要求时才 `git push`；否则只本地提交。

---

## 1. 构建与验证

统一用一键脚本，不要再手写 build/smoke 组合：

```powershell
.\tools\check.ps1              # 构建 + 对比基线（冒烟 16s）
.\tools\check.ps1 -Fast        # 冒烟 8s（纯注释/改名够用）
.\tools\check.ps1 -NoBuild     # 跳过构建（会校验 exe 不比源码旧）
.\tools\check.ps1 -Baseline    # 重写基线（判定行为变化可接受时）
.\tools\check.ps1 -Quiet       # 只打一行结论
```

- 基线：`bench\baseline.txt`，9 项指标（selftest / tps ×2 / blocks / chunks / warmup ×3 / mods）。
- **基线不通过就停下**，不要继续下一步；查明原因或回退。
- `-Fast` 只跑 8 秒，`tps_business` 尚未收敛（容差 ±0.5 覆盖）；性能相关改动仍用默认 16 秒。
- 冒烟日志写在 `build\smoke-logs\`（已在 `.gitignore` 的 `build/` 内）。

## 2. 提交纪律

- **一步一提交**，每个提交必须能独立回退。
- 提交信息写进 `build\commitmsg*.txt`（UTF-8），再 `git -c i18n.commitEncoding=UTF-8 commit -F build/commitmsg*.txt`。
  **不要**用 `-m "..."` 塞中文引号，会打断命令行。
- 提交前必须已通过 `check.ps1`；提交信息里写清「现状证明 / 最小改法 / 验证手段」。
- 提交后用 `git status --short` 确认工作树干净，并删掉临时的 `commitmsg` 文件。

## 3. 工作边界

- **`src/render/**` 允许按既定边界增量拆分**（计划见 `docs\DOCS自制版\架构决策-渲染层拆分.md`）：
  新功能不得直接加进 `VulkanContext`；改动碰到某职责时顺手抽出独立类（单次 ≤300 行）。
  禁止无关的大规模改名/搬文件、禁止跨层反向依赖。
- 引擎分层已固定，新增代码沿用四对结构：

  | 线程 | 数据层 | 逻辑层 |
  |---|---|---|
  | 业务 | `business::World` | `business::BusinessWorld` |
  | 物理 | `physics::PhysicsState` | `physics::PhysicsWorld` |
  | 生成 | `worldgen::StreamState` | `worldgen::ChunkStreamer` |
  | 输入 | `input::InputState` | `input::InputSampler` |

- 跨线程只走 `SPSCQueue`（事件）或原子槽（状态），不要共享裸指针。
- 命名遵循 `docs\DOCS自制版\重构计划.md` 的 R1–R10。

## 4. 记录约定

| 文件 | 用途 | 规则 |
|---|---|---|
| `docs\bug记录.md` | 缺陷台账 | **只记录不修**（除非用户要求）；每条带严重度与核实结论 |
| `docs\性能问题记录.md` | 性能问题 P1–P14 | 只记录不修 |
| `docs\DOCS自制版\重构计划.md` | 重构规则与顺序 | 用户维护，按需引用 |
| `docs\通读记录\` | 用户的通读笔记 | **gitignored，不要提交、不要改** |

## 5. 已知的工具坑

- **改共享结构体 / 改会改变对象布局的头之后，必须全量重编**。
  涉及：`shared/protocol/Protocol.hpp`、`shared/state/**`、`shared/block/**`，
  以及任何被多个 TU 包含的头（如 `render/VulkanContext.hpp`）。
  漏了的话，增量构建会留下仍用旧布局的 `.obj` → 队列里 `T` 的大小在生产者/消费者两边不一致
  → 堆损坏或启动后直接崩（表现：`xmemory` 里 `invalid argument`、
  在「命令池 + 命令缓冲就绪」之后进程消失、`LNK2005` 多重定义；2026-11-24 一天内出现 4 次）。
  处理：删掉 `build\Debug\CMakeFiles\sandbox_engine.dir` 再构建。

- `tools\smoke.ps1` **必须保存为 UTF-8 with BOM**：Windows PowerShell 5.1 会把无 BOM 的 UTF-8 当 GBK，中文注释导致解析失败。
- `git commit -m` 带中文引号会报 `pathspec` 错误 → 用 `-F` 文件。
- 沙箱下曾在 `build\` 出现「写入被拒但 ACL 正常」的情况；本会话已切到完全权限，若再次出现请如实报告，不要反复重试。

## 6. 当前主线

1. **接手 Jolt**（两天后）：逻辑层 `physics\PhysicsWorld.cpp` 整段替换积分/碰撞；数据层 `physics\PhysicsState.hpp` 的 `RigidBody` 换成 `JPH::BodyID` + 位置缓存。
   **已定（2026-11-23）：刚体按「多刚体 + 约束表」建** —— 一个结构 → N 个 `PhysicsBody` + `PhysicsConstraint` 表，1.0 只用一个刚体、约束表为空；`contraptionId` 语义变为**结构 id**。详见 `docs\七大维度与一大底座设计\09-模组化边界.md` §九。
2. ~~**进场前先处理 `P1`**~~ ✅ 已完成（实测稳态 0 次/秒全量快照，见 `性能问题记录.md`）。
3. ~~**区块边缘透空**~~ ✅ 已销案：真实根因是相机 UBO 竞态（已由 push constant 修复），
   B135 所指路径已无调用者（死代码已删），B136 经核实不是缺陷。见 `bug记录.md` 第八节。
4. **存档系统**（原型已完成，2026-11-26）：格式 + 读写 + 流式接缝，四步全部落地，
   设计见 `docs\DOCS自制版\存档系统设计.md`。多存档命名/选择是下一步（目录结构已就位）。

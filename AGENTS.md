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

- **`src/render/**` 正在增量开发，不重构、不改名、不搬文件**（除非用户明确要求）。
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

- `tools\smoke.ps1` **必须保存为 UTF-8 with BOM**：Windows PowerShell 5.1 会把无 BOM 的 UTF-8 当 GBK，中文注释导致解析失败。
- `git commit -m` 带中文引号会报 `pathspec` 错误 → 用 `-F` 文件。
- 沙箱下曾在 `build\` 出现「写入被拒但 ACL 正常」的情况；本会话已切到完全权限，若再次出现请如实报告，不要反复重试。

## 6. 当前主线

1. **接手 Jolt**（两天后）：逻辑层 `physics\PhysicsWorld.cpp` 整段替换积分/碰撞；数据层 `physics\PhysicsState.hpp` 的 `RigidBody` 换成 `JPH::BodyID` + 位置缓存。
2. **进场前先处理 `P1`**：`syncWorldToPhysics(world_.renderSnapshot())` 每 tick 无条件拍全量快照（10k chunk 时峰值近 1GB）。
3. **区块边缘透空**（`bug记录` 第八节 B135/B136 + `P14`）待验证后修，修法顺序：脏集合改并集 → 快照按 chunk 分组 → 重建范围对齐 1-ring → 延迟销毁 buffer。

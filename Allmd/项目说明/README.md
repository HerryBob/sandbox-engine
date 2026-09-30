# 沙盒引擎

C++ 自研方块沙盒引擎（对标 Minecraft），核心创新：
**服务端权威逻辑 + 客户端渲染分离**、**三轨时间机制**、**玩家自由拼接方块组成的动态复合体**。

> 详细架构设计见 `../架构与设计/沙盒引擎架构设计文档.md`。

## 当前阶段：原型核心玩法闭环（三轨消息流 + 碰撞 + 复合体改造）

对应架构文档 7.5 开发顺序第 1~5 步 + 7.6 ①定向清理 + ②数据层第一项：三线程 ✅、真实消息流 ✅、Block/Contraption ✅、刚体积分 ✅、碰撞/裁决 ✅、接触语义 ✅、飞船改造 ✅、伪渲染 ✅、定向清理 ✅。

当前已实现：
- `core/TimeManager`：时间系数 = **global × 域乘数**（业务/物理各一，默认 1），服务端权威单一写者
- `core/SPSCQueue`：单生产者-单消费者无锁环形队列（业务↔物理、输入→物理），哨兵格满/空判定
- `core/ThreadLoop`：线程封装（生命周期四件套：构造/拷贝删除/移动删除/析构自停）
- `core/Log`：分级日志（`LogLevel` Debug/Info/Warn/Error + `g_logLevel` + `log()` 原子输出 + `LOG_*` 宏）
- `input/InputController`：玩家操控权使能开关（服务端权威），禁用时输入仍墙钟采样但不施力（"玩家无法移动"）
- 业务线程 **20TPS**、物理线程 **25TPS**（accumulator 累积驱动）、输入线程墙钟采样（**永不设时间系数**）
- **三轨消息流**：业务每 tick 下发指令 → 物理真处理 → 位置快照合并回传（回执逐条 vs 快照合并语义区分，命令零丢失）
- **方块放置/破坏裁决**：业务裁决（存在性/类型权威）→ 自动排队指令 → 物理增删碰撞体；重复放置、破坏不存在方块被拒
- **复合体打包/解包**：业务发打包指令 → 物理建 `RigidBody{id,pos,vel,blockCount}` → 回执确认（世界坐标方块列表，解包重建业务侧）
- **刚体积分**：半隐式欧拉（pos += vel×dt），推力来自输入线程墙钟采样
- **碰撞检测**：空间哈希粗相 + 严格三轴区间重叠窄相（贴边不算碰撞）；撞上回退 + 清速 + CollisionEvent 回传业务裁决破坏
- **碰撞事件 = 接触语义**（2026-09-30）：物理侧维护接触集合（刚体 id → 接触中方块），Started 只在接触开始时发（同一障碍物只报 1 次，断言 ==1）；持续重叠不发；接触结束/方块删除后移除
- **位置带时间戳 = 插值基座**（2026-09-30）：业务侧双槽快照（`PosSample{simT,x,y,z}` 恒存最近两个）+ `sampleShipPos(simT)` lerp 插值（窗口外钳制）；渲染 60Hz+/无限帧率每帧按渲染时刻插值（帧率越高插值点越密，机制与帧率无关）。顺带修复：物理位置快照原本每墙钟循环重复发（同 simT 发 ~15 次），改为**只在模拟时刻推进时发**（每模拟 tick 一条）
- **飞船改造 + 增量最小碰撞箱**：刚体内方块增删（Sable 思想简化落地），`recomputeAABB` 实时贴合（半宽/中心偏移）
- **伪渲染**：`demo/ConsoleRenderer` 控制台侧视 ASCII 帧（第 4 轨只读，视口跟随飞船），渲染线程落地前的数据通路验证
- `shared/Block`：BlockType/BlockPos/Block（方块数据层，int 格索引 + 格心 +0.5 语义），初始世界 3×3×3 木头飞船 + 石头障碍物
- 单机模式：客户端+服务端同进程，进程内无锁队列通信

## 代码结构（定向清理后）

```
├── CMakeLists.txt          # CMake 工程根（GLOB_RECURSE 自动收编新文件）
├── CMakePresets.json       # Debug / Release 两套配置
├── src/
│   ├── main.cpp            # demo 入口：runDemo() + main()，退出码 0/1
│   ├── core/               # TimeManager / SPSCQueue / ThreadLoop / Log
│   ├── business/           # 业务世界 20TPS（纯能力化，无演示逻辑）
│   ├── physics/            # 物理世界 25TPS（RigidBody / 碰撞体 / 碰撞）
│   ├── input/              # 独立输入线程（墙钟采样）
│   ├── shared/             # Protocol（消息协议）、Block（方块数据）
│   └── demo/               # DemoDriver（tick 事件源）/ ConsoleRenderer（伪渲染）
├── bench/                  # baseline.txt 性能基线（Release 版输出）
└── tests/                  # 单元测试（后置）
```

## 构建（VS2026 打开文件夹）

1. VS2026 → 文件 → 打开 → 文件夹 → 选中本目录（含 `CMakeLists.txt` 这层）
2. 顶部配置下拉选 `Debug · 三轨调试`（或 Release）
3. 生成 + 运行 `sandbox_engine`

命令行（已装 VS2026 自带 CMake/Ninja）：
```
call "E:\VS2026\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset debug
cmake --build build/debug
build\debug\sandbox_engine.exe
```

Release + 基线：
```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
build\release\sandbox_engine.exe > bench\baseline.txt   # 20/20 软断言全过
```

## 里程碑

- [x] 三轨空转骨架（三线程 + time_scale + 无锁队列）
- [x] 输入线程接入物理 tick（采样→队列→消费），输入禁用（InputController）
- [x] 三轨真实消息流（命令 / 快照合并回传 / 回执语义）
- [x] Block 最小模型（BlockType/BlockPos/Block，3×3×3 木头飞船）
- [x] Contraption 打包/解包（RigidBody 建/拆 + 回执确认）
- [x] 物理刚体积分（pos += vel×dt，半隐式欧拉）
- [x] 推力操控（输入墙钟采样 → 物理施力；InputController.enabled 可禁玩家）
- [x] 方块放置/破坏裁决（业务裁决 → 物理执行，拒绝重复/不存在）
- [x] 碰撞检测（空间哈希粗相 + 严格区间重叠窄相）+ 碰撞事件回传业务破坏
- [x] 碰撞事件 = 接触语义（Started 只在接触开始时发，同一障碍物只报 1 次）
- [x] 位置带时间戳 = 插值基座（业务双槽快照 + sampleShipPos lerp；顺带修复物理位置快照洪水）
- [x] 解包闭环（物理回执携带世界坐标方块列表，业务重建）
- [x] 飞船改造（刚体内方块增删）+ 增量最小碰撞箱（Sable 思想简化）
- [x] 伪渲染（ConsoleRenderer 控制台侧视帧，第 4 轨只读）
- [x] 定向清理（DemoDriver / 日志分级 / release + 基线，20/20 软断言）
- [ ] 世界数据升级（区块化 chunk / 变更事件流，碰撞去重 + 位置时间戳已 ✅）
- [ ] 极简 TE（业务线程方块逻辑）
- [ ] 极简渲染（约 5.x，引入 vcpkg + Vulkan + glfw；渲染=第四轨只读快照）
- [ ] 模组类拦截系统、子复合体、增量更新、结构冻结等（全部后置）

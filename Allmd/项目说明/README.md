# 沙盒引擎

C++ 自研方块沙盒引擎（对标 Minecraft），核心创新：
**服务端权威逻辑 + 客户端渲染分离**、**三轨时间机制**、**玩家自由拼接方块组成的动态复合体**。

> 详细架构设计见 `../架构与设计/沙盒引擎架构设计文档.md`。

## 当前阶段：原型最小核心（三轨消息流 + 最小业务世界）

对应架构文档 7.5 开发顺序第 1~2 步：三线程跑通 ✅、三轨真实消息流 ✅、Block 与 Contraption 打包/解包 ✅。

当前已实现：
- `core/TimeManager`：时间系数 = **global × 域乘数**（业务/物理各一，默认 1），服务端权威单一写者
- `core/SPSCQueue`：单生产者-单消费者无锁环形队列（业务↔物理、输入→物理）
- `core/ThreadLoop`：线程封装
- `input/InputController`：玩家操控权使能开关（服务端权威），禁用时输入仍墙钟采样但不施力（"玩家无法移动"）
- 业务线程 **20TPS**、物理线程 **25TPS**（accumulator 累积驱动）、输入线程墙钟采样（**永不设时间系数**）
- **三轨消息流**：业务每 tick 下发 MoveBlock 命令 → 物理真处理 → 位置快照合并回传（回执 vs 快照语义区分）
- `shared/Block`：BlockType/BlockPos/Block（方块数据层），初始世界一艘 3×3×3 木头飞船（27 块）
- **Contraption 打包/解包**：业务发打包指令 → 物理建 `RigidBody{id,pos,vel,blockCount}` → 回执确认；解包拆除刚体（物理形态权威转移）
- **刚体积分 + 推力操控**（2.3/2.3.1）：半隐式欧拉 `v+=a×dt; x+=v×dt`，输入事件真正施力，飞船可加速/滑行；软断言 6/6 通过
- 单机模式：客户端+服务端同进程，进程内无锁队列通信

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

## 目录结构

```
├── CMakeLists.txt          # CMake 工程根
├── CMakePresets.json       # Debug / Release 两套配置
├── src/
│   ├── main.cpp            # 组装三轨，入口
│   ├── core/               # 线程、时间管理器、无锁队列
│   ├── business/           # 业务世界 20TPS（Block、打包状态机）
│   ├── physics/            # 物理世界 25TPS（RigidBody、刚体表）
│   ├── input/              # 独立输入线程（墙钟采样）
│   ├── shared/             # Protocol（消息协议）、Block（方块数据）
│   └── render/             # 极简渲染（后置）
└── tests/                  # 单元测试（后置）
```

## 里程碑

- [x] 三轨空转骨架（三线程 + time_scale + 无锁队列）
- [x] 输入线程接入物理 tick（采样→队列→消费），输入禁用（InputController）
- [x] 三轨真实消息流（MoveBlock 命令 / 快照合并回传 / 回执语义）
- [x] Block 最小模型（BlockType/BlockPos/Block，3×3×3 木头飞船）
- [x] Contraption 打包/解包（RigidBody 建/拆 + 回执确认）
- [x] 刚体积分 + 推力操控（半隐式欧拉 v+=a×dt；输入→物理闭环，软断言 6/6）
- [ ] 物理刚体积分（pos += vel×dt，2.3）
- [ ] 极简 TE（业务线程方块逻辑）
- [ ] 最小物理世界：单 AABB 碰撞、半隐式欧拉积分
- [ ] 世界存储/区块（Block/BlockState 分离、坐标映射）
- [ ] 极简渲染（约 5.x，引入 vcpkg + Vulkan + glfw；渲染=第四轨只读快照）
- [ ] 模组类拦截系统、子复合体、增量更新、结构冻结等（全部后置）

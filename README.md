# 沙盒引擎（Sandbox Engine）

C++ 自研方块沙盒引擎（对标 Minecraft），核心创新：

- **服务端权威逻辑 + 客户端渲染分离**：服务端只管逻辑与物理，渲染/输入采集全部放客户端
- **三轨时间机制**：业务线程 20TPS + 物理线程 25TPS + 输入线程墙钟原生（永不缩放），时间系数 = global × 域乘数，可实时慢放/加速
- **动态复合体**：玩家把任意方块拼成的结构打包成单个刚体，交物理线程模拟（对标机械动力航空学）

## 当前阶段：原型核心玩法闭环

三轨消息流 + 方块放置/破坏裁决 + 复合体打包飞行 + 碰撞破坏 + 飞船改造 + 伪渲染：

- [x] 三轨空转骨架（线程/时间/无锁队列）
- [x] 三轨真实消息流（命令 / 快照合并 / 回执语义）
- [x] Block 最小模型（3×3×3 飞船）
- [x] Contraption 打包/解包（RigidBody 建/拆）
- [x] 刚体积分（半隐式欧拉，飞船飞行）
- [x] 推力操控（输入→物理闭环，InputController 可禁玩家）
- [x] 方块放置/破坏裁决（业务裁决 → 物理执行，重复放置/破坏不存在被拒）
- [x] 碰撞检测（空间哈希粗相 + 严格区间重叠窄相）→ 碰撞事件回传业务破坏
- [x] 碰撞事件 = 接触语义（Started 只在接触开始时发，同一障碍物只报 1 次，断言 ==1）
- [x] 位置带时间戳 = 插值基座（双槽快照 + sampleShipPos lerp；修复物理位置快照洪水）
- [x] 区块化存储（16³ chunk 稀疏映射 + 区域查询 + 修改跟踪，空 chunk 不占内存）
- [x] 变更事件流（业务→渲染增量通道，tick 聚合脏 chunk；**数据层 ② 收官**）
- [x] 飞船改造（刚体内方块增删）+ 增量最小碰撞箱（半宽/偏移实时贴合）
- [x] 伪渲染（ConsoleRenderer 控制台侧视帧，第 4 轨只读）
- [x] 定向清理（DemoDriver 拆演示 / 日志分级 / release 基线 bench/baseline.txt，25/25 软断言）
- [ ] 极简 TE / 极简渲染（Vulkan，约 5.x）

## 构建

VS2026（自带 CMake + Ninja）打开文件夹，或命令行：

```
call "E:\VS2026\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset debug
cmake --build build/debug
build\debug\sandbox_engine.exe
```

Release 构建（性能基线，Debug /RTC1 会扭曲性能 5-10 倍）：

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
build\release\sandbox_engine.exe > bench\baseline.txt
```

> 详细架构设计见 `Allmd/架构与设计/沙盒引擎架构设计文档.md`
> 项目说明见 `Allmd/项目说明/README.md`

## 状态

原型阶段，私有仓库，仅个人开发。公开只读前暂不接受 PR。

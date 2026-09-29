// 沙盒引擎 · 单机原型入口（demo 模式）
// 对应架构文档 7.5 步骤 1：先跑通三线程（业务20TPS / 物理25TPS / 输入墙钟）
// 单机模式：客户端+服务端同进程，进程内无锁队列通信。
// 本文件 = demo 入口（装配引擎 + 编排阶段演示 + 软断言汇总）；
// 引擎本体在 core/business/physics/input，演示/伪渲染在 demo/。
#include "business/BusinessWorld.hpp"
#include "core/Log.hpp"
#include "core/SPSCQueue.hpp"
#include "core/ThreadLoop.hpp"
#include "core/TimeManager.hpp"
#include "demo/ConsoleRenderer.hpp"
#include "demo/DemoDriver.hpp"
#include "input/InputController.hpp"
#include "input/InputSampler.hpp"
#include "physics/PhysicsWorld.hpp"
#include "shared/Protocol.hpp"

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

// 单机 Windows 原型：把控制台输出切到 UTF-8。
// 程序以 /utf-8 输出 UTF-8 字节，若控制台停留 GBK(936) 代码页会导致中文乱码。
static void initConsole() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
}

using namespace engine;

// demo 模式主流程：三轨启动 → time_scale 阶段演示（穿插伪渲染帧）→
// 优雅停止 → 软断言汇总。返回 0 = 全部断言通过，非 0 = 有失败。
static int runDemo() {
    initConsole();
    std::cout << "=== 沙盒引擎 · 三轨消息流 + 刚体积分 + 增量碰撞箱 + 伪渲染骨架 ===\n";

    // 全局 time_scale 单实例，三条轨共享
    core::TimeManager tm;
    // 进程内无锁队列（单机模式，见文档 1.3）
    core::SPSCQueue<shared::InputEvent>     inputToPhysics(512);
    core::SPSCQueue<shared::PhysicsCommand> businessToPhysics(512);
    core::SPSCQueue<shared::PhysicsResult>  physicsToBusiness(512);

    business::BusinessWorld businessWorld(tm);
    physics::PhysicsWorld   physicsWorld(tm);
    input::InputSampler     inputSampler;
    input::InputController  inputController; // 玩家操控权开关（服务端权威）
    engine::demo::DemoDriver demoDriver(businessWorld); // tick 事件源（演示）

    // 初始世界内容（2.1 验证）：一艘 3×3×3 木头飞船 + 一块路径上的石头障碍物
    std::cout << "初始世界：方块=" << businessWorld.blockCount()
              << " 飞船范围=(-1,-1,-1)~(1,1,1) 材质=Wood"
              << " 障碍物=石头(50,0,0)\n";

    core::ThreadLoop businessLoop("business");
    core::ThreadLoop physicsLoop("physics");
    core::ThreadLoop inputLoop("input");

    // ---- 业务线程 20TPS：固定 50ms 墙钟节奏 ----
    businessLoop.start([&] {
        auto next = std::chrono::steady_clock::now();
        while (!businessLoop.shouldStop()) {
            businessWorld.advanceTick(tm.effectiveScale(core::Domain::Business));
            demoDriver.step();                             // 演示事件源（tick 驱动）
            businessWorld.syncToPhysics(businessToPhysics);       // 发指令 → 物理
            businessWorld.receiveFromPhysics(physicsToBusiness);  // 收结果 ← 物理
            next += std::chrono::milliseconds(50);
            std::this_thread::sleep_until(next);
        }
    });

    // ---- 物理线程 25TPS：accumulator 累积驱动（文档 2.2）----
    physicsLoop.start([&] {
        auto prev = std::chrono::steady_clock::now();
        while (!physicsLoop.shouldStop()) {
            const auto now = std::chrono::steady_clock::now();
            const double wallDt =
                std::chrono::duration<double>(now - prev).count();
            prev = now;
            physicsWorld.advance(tm.effectiveScale(core::Domain::Physics), wallDt,
                                 inputToPhysics, businessToPhysics);
            physicsWorld.syncToBusiness(physicsToBusiness);
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 避免忙等
        }
    });

    // ---- 输入线程：墙钟采样，完全不受 time_scale ----
    inputLoop.start([&] {
        auto prev = std::chrono::steady_clock::now();
        while (!inputLoop.shouldStop()) {
            const auto now = std::chrono::steady_clock::now();
            const double wallNow =
                std::chrono::duration<double>(now.time_since_epoch()).count();
            inputSampler.sample(wallNow, inputController, inputToPhysics);
            std::this_thread::sleep_for(std::chrono::milliseconds(8)); // ~125Hz
        }
    });

    // ---- 主线程：阶段演示 time_scale 效果 ----
    // global 同时缩放业务+物理；域乘数只缩放单轨（见 TimeManager）
    // 伪渲染穿插：把 sleep 拆成 1s 块，每块画一帧 ASCII 侧视图
    //（第 4 轨只读：renderSnapshot 锁内拷贝，渲染输出整帧持 g_logMutex 原子化）。
    auto renderWait = [&](double seconds) {
        for (int f = 0; f < static_cast<int>(seconds); ++f) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            demo::renderFrame(businessWorld.renderSnapshot(), businessWorld.shipX(),
                              businessWorld.contraptionState(),
                              static_cast<int>(businessWorld.tickCount()));
        }
    };
    auto stage = [&](double globalScale, const char* label) {
        tm.setGlobal(globalScale);
        std::cout << "\n===== global time_scale = " << globalScale
                  << "（" << label << "）=====\n";
        renderWait(3);
        std::lock_guard<std::mutex> lock(core::g_logMutex);
        std::cout << "  -- 业务sim=" << businessWorld.simTime()
                  << "s 物理sim=" << physicsWorld.simTime()
                  << "s 输入采样=" << inputSampler.samples() << "\n";
    };

    stage(1.0, "正常");
    const double shipAtT3 = businessWorld.shipX();   // 采样点①：正常阶段末（~3s，打包后0.5s）
    stage(0.5, "减速·输入应流畅");
    const double shipAtT6 = businessWorld.shipX();   // 采样点②：减速阶段末（~6s）
    // 飞船改造采样①（tick80 已执行且物理已处理）：刚体 28 块、AABB 增量扩张
    const long long bodyBlocksAtT6 = physicsWorld.bodyBlockCount();
    const double    bodyHalfAtT6   = physicsWorld.bodyHalfExtentX();
    const double    bodyOffsetAtT6 = physicsWorld.bodyCenterOffsetX();
    stage(2.0, "加速");
    const double shipAtT9 = businessWorld.shipX();   // 采样点③：加速阶段末（~9s）
    // 飞船改造采样②（tick160 已执行且物理已处理）：刚体 27 块、AABB 增量收缩
    const long long bodyBlocksAtT9 = physicsWorld.bodyBlockCount();
    const double    bodyHalfAtT9   = physicsWorld.bodyHalfExtentX();
    const double    bodyOffsetAtT9 = physicsWorld.bodyCenterOffsetX();

    // 域乘数验证：单独把物理轨慢放到 0.5，业务轨不受影响
    tm.setGlobal(1.0);
    tm.setDomainScale(core::Domain::Physics, 0.5);
    std::cout << "\n===== 物理域乘数 = 0.5（仅物理轨慢放，业务不受影响）=====\n";
    renderWait(3);
    {
        std::lock_guard<std::mutex> lock(core::g_logMutex);
        std::cout << "  -- 业务sim=" << businessWorld.simTime()
                  << "s 物理sim=" << physicsWorld.simTime()
                  << "s 输入采样=" << inputSampler.samples() << "\n";
    }
    tm.setDomainScale(core::Domain::Physics, 1.0); // 恢复
    const double shipAtT12 = businessWorld.shipX();  // 采样点④：域乘数阶段末（~12s）

    // 输入使能验证：禁用玩家操控权后，输入仍墙钟采样，但物理不再施力（玩家无法操控）
    inputController.setEnabled(false);
    std::cout << "\n===== 输入禁用（玩家无法操控；事件仍采样但无效）=====\n";
    renderWait(3);
    {
        std::lock_guard<std::mutex> lock(core::g_logMutex);
        std::cout << "  -- 业务sim=" << businessWorld.simTime()
                  << "s 物理sim=" << physicsWorld.simTime()
                  << "s 输入采样=" << inputSampler.samples()
                  << " 物理有效输入=" << physicsWorld.inputValid() << "\n";
    }
    inputController.setEnabled(true); // 恢复
    const double shipAtT15 = businessWorld.shipX();  // 采样点⑤：禁用阶段末（~15s，已解包冻结）

    // 优雅停止三轨
    businessLoop.stop();
    physicsLoop.stop();
    inputLoop.stop();

    std::cout << "\n===== 汇总 =====\n"
              << "业务 sim=" << businessWorld.simTime()
              << "s tick=" << businessWorld.tickCount()
              << " 命令发送=" << businessWorld.commandsSent()
              << " 结果接收=" << businessWorld.resultsReceived()
              << " 方块=(" << businessWorld.blockX() << ","
              << businessWorld.blockY() << "," << businessWorld.blockZ() << ")"
              << " 飞船=(" << businessWorld.shipX() << ","
              << businessWorld.shipY() << "," << businessWorld.shipZ() << ")"
              << " 放置/破坏=" << businessWorld.blocksPlaced() << "/"
              << businessWorld.blocksRemoved()
              << "(拒" << businessWorld.placementsRejected() << "/"
              << businessWorld.removalsRejected() << ")"
              << " 碰撞=" << businessWorld.collisions()
              << " 物理碰撞体=" << businessWorld.physicsBlockCount() << "\n"
              << "物理 sim=" << physicsWorld.simTime()
              << "s tick=" << physicsWorld.tickCount()
              << " 消费输入=" << physicsWorld.inputConsumed()
              << " 有效输入=" << physicsWorld.inputValid()
              << " 处理指令=" << physicsWorld.processedCommands()
              << " 刚体=" << physicsWorld.bodyCount()
              << " 碰撞体=" << physicsWorld.colliderCount()
              << " 碰撞=" << physicsWorld.collisions()
              << " 方块=(" << physicsWorld.blockX() << ","
              << physicsWorld.blockY() << "," << physicsWorld.blockZ() << ")\n"
              << "输入 采样=" << inputSampler.samples() << "\n"
              << "世界 方块=" << businessWorld.blockCount()
              << " 刚体方块=" << physicsWorld.bodyBlockCount()
              << "（飞船改造后：28→27，碰撞箱实时贴合）"
              << " 打包状态=" << businessWorld.contraptionState()
              << "（0=未打包 1=已打包 2=已解包）\n";

    // ---- 软断言（2.3.1）：程序自己核对关键行为，替代纯人眼 ----
    // 阈值故意宽松，容忍每次运行的墙钟调度微差；判错 = 行为确实坏了。
    bool allPass = true;
    auto check = [&](bool ok, const char* what) {
        allPass = allPass && ok;
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << "\n";
    };

    std::cout << "\n===== 软断言 =====\n"
              << "  飞船采样: T3=" << shipAtT3 << " T6=" << shipAtT6
              << " T9=" << shipAtT9 << " T12=" << shipAtT12
              << " T15=" << shipAtT15 << "\n"
              << "  刚体采样: T6块数=" << bodyBlocksAtT6
              << " 半宽=" << bodyHalfAtT6 << " 偏移=" << bodyOffsetAtT6
              << " | T9块数=" << bodyBlocksAtT9
              << " 半宽=" << bodyHalfAtT9 << " 偏移=" << bodyOffsetAtT9 << "\n";

    check(shipAtT15 > 10.0, "飞船完成打包-飞行-解包全程（总位移>10 格）");
    check(shipAtT6 > shipAtT3 + 0.2,
          "减速阶段推力仍生效（输入墙钟，飞船继续前进）");
    check((shipAtT9 - shipAtT6) > (shipAtT6 - shipAtT3) * 1.5,
          "加速阶段位移增量显著大于减速阶段（推力被 time_scale 缩放）");
    check(physicsWorld.processedCommands() == businessWorld.commandsSent(),
          "命令零丢失（物理处理数 == 业务发送数）");
    check(businessWorld.contraptionState() == 2,
          "解包完成（打包状态=2）");
    check(businessWorld.blockCount() == 27,
          "最终世界 27 块（飞船 27 + 障碍物已被撞击破坏）");

    // 方块放置/破坏（2.7 裁决）+ 飞船改造（刚体内方块增删，3.4 演进）：
    check(businessWorld.blocksPlaced() == 2 && businessWorld.placementsRejected() == 1,
          "合法放置 2 次成功（主世界 1 + 刚体改造 1），重复放置被拒 1 次");
    check(businessWorld.blocksRemoved() == 2 && businessWorld.removalsRejected() == 1,
          "合法破坏 2 次成功（主世界 1 + 刚体改造 1），破坏不存在方块被拒 1 次");
    check(businessWorld.findBlock(engine::shared::BlockPos{0, 5, 0}) == nullptr,
          "放置又破坏后 (0,5,0) 已不存在（裁决闭环）");
    check(businessWorld.physicsBlockCount() ==
              static_cast<long long>(businessWorld.blockCount()),
          "物理碰撞体数 == 业务方块数（两侧一致性）");

    // 飞船改造 + 增量最小碰撞箱（Sable 思想）：
    check(bodyBlocksAtT6 == 28 &&
              std::abs(bodyHalfAtT6 - 2.0) < 1e-9 &&
              std::abs(bodyOffsetAtT6 - 0.5) < 1e-9,
          "飞船改造：tick80 后刚体 28 块、AABB 半宽 2.0、偏移 0.5（增量扩张贴合）");
    check(bodyBlocksAtT9 == 27 &&
              std::abs(bodyHalfAtT9 - 1.5) < 1e-9 &&
              std::abs(bodyOffsetAtT9 - 0.0) < 1e-9,
          "飞船改造：tick160 后刚体 27 块、AABB 半宽 1.5、偏移 0.0（增量收缩贴合）");

    // 单 AABB 碰撞（4.3）：飞船撞上飞行路径障碍物 → 碰撞事件 → 业务裁决破坏
    check(businessWorld.collisions() >= 1,
          "飞船撞上障碍物（碰撞事件 ≥1）");
    {
        // 障碍物破坏闭环：碰撞后 (50,0,0) 处不再有石头。
        // 注意：飞船撞破障碍物继续飞，解包后其右舷方块可能恰好落进 (50,0,0)
        // （木头），因此不能断言"该格无方块"，只能断言"该格无石头"。
        const auto* obs = businessWorld.findBlock(engine::shared::BlockPos{50, 0, 0});
        check(obs == nullptr || obs->type != engine::shared::BlockType::Stone,
              "障碍物（石头）已被撞击破坏（(50,0,0) 无石头）");
    }
    check(shipAtT15 > 45.0,
          "飞船撞破障碍物后继续飞（未停在碰撞点）");

    // 解包闭环（2.7）：业务 blocks_ 已重建为物理回写的世界坐标（飞船位置附近），
    // 而非打包前的局部坐标 (-1..1)——两侧坐标不再错位。
    {
        const double sx = businessWorld.shipX();
        bool nearShip = true;
        for (const auto& b : businessWorld.blocks()) {
            if (std::abs(b.pos.x - sx) > 2.5) nearShip = false;
        }
        check(nearShip && businessWorld.blockCount() == 27,
              "解包后业务方块重建到飞船位置（世界坐标，非局部坐标）");
    }

    std::cout << (allPass ? ">>> 全部断言通过\n" : ">>> 存在失败断言！\n");

    std::cout << "三轨消息流 + 刚体积分 + 增量碰撞箱 + 伪渲染骨架运行结束。\n";
    return allPass ? 0 : 1; // 退出码可被脚本/CI 使用：0=全部断言通过
}

int main() {
    initConsole();
    return runDemo();
}

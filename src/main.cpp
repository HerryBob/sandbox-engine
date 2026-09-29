// 沙盒引擎 · 单机原型入口
// 对应架构文档 7.5 步骤 1：先跑通三线程（业务20TPS / 物理25TPS / 输入墙钟）
// 单机模式：客户端+服务端同进程，进程内无锁队列通信。
#include "business/BusinessWorld.hpp"
#include "core/Log.hpp"
#include "core/SPSCQueue.hpp"
#include "core/ThreadLoop.hpp"
#include "core/TimeManager.hpp"
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

int main() {
    initConsole();
    std::cout << "=== 沙盒引擎 · 三轨消息流 + 刚体积分骨架 ===\n";

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

    // 初始世界内容（2.1 验证）：一艘 3×3×3 木头飞船
    std::cout << "初始世界：方块=" << businessWorld.blockCount()
              << " 飞船范围=(-1,-1,-1)~(1,1,1) 材质=Wood\n";

    core::ThreadLoop businessLoop("business");
    core::ThreadLoop physicsLoop("physics");
    core::ThreadLoop inputLoop("input");

    // ---- 业务线程 20TPS：固定 50ms 墙钟节奏 ----
    businessLoop.start([&] {
        auto next = std::chrono::steady_clock::now();
        while (!businessLoop.shouldStop()) {
            businessWorld.advanceTick(tm.effectiveScale(core::Domain::Business));
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
    auto stage = [&](double globalScale, const char* label) {
        tm.setGlobal(globalScale);
        std::cout << "\n===== global time_scale = " << globalScale
                  << "（" << label << "）=====\n";
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::lock_guard<std::mutex> lock(core::g_logMutex);
        std::cout << "  -- 业务sim=" << businessWorld.simTime()
                  << "s 物理sim=" << physicsWorld.simTime()
                  << "s 输入采样=" << inputSampler.samples() << "\n";
    };

    stage(1.0, "正常");
    const double shipAtT3 = businessWorld.shipX();   // 采样点①：正常阶段末（~3s，打包后0.5s）
    stage(0.5, "减速·输入应流畅");
    const double shipAtT6 = businessWorld.shipX();   // 采样点②：减速阶段末（~6s）
    stage(2.0, "加速");
    const double shipAtT9 = businessWorld.shipX();   // 采样点③：加速阶段末（~9s）

    // 域乘数验证：单独把物理轨慢放到 0.5，业务轨不受影响
    tm.setGlobal(1.0);
    tm.setDomainScale(core::Domain::Physics, 0.5);
    std::cout << "\n===== 物理域乘数 = 0.5（仅物理轨慢放，业务不受影响）=====\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));
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
    std::this_thread::sleep_for(std::chrono::seconds(3));
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
              << businessWorld.shipY() << "," << businessWorld.shipZ() << ")\n"
              << "物理 sim=" << physicsWorld.simTime()
              << "s tick=" << physicsWorld.tickCount()
              << " 消费输入=" << physicsWorld.inputConsumed()
              << " 有效输入=" << physicsWorld.inputValid()
              << " 处理指令=" << physicsWorld.processedCommands()
              << " 刚体=" << physicsWorld.bodyCount()
              << " 方块=(" << physicsWorld.blockX() << ","
              << physicsWorld.blockY() << "," << physicsWorld.blockZ() << ")\n"
              << "输入 采样=" << inputSampler.samples() << "\n"
              << "世界 方块=" << businessWorld.blockCount()
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
              << " T15=" << shipAtT15 << "\n";

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
          "打包/解包往返后方块不丢（世界仍 27 块）");

    std::cout << (allPass ? ">>> 全部断言通过\n" : ">>> 存在失败断言！\n");

    std::cout << "三轨消息流 + 刚体积分骨架运行结束。\n";
    return 0;
}

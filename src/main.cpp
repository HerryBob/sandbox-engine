// 沙盒引擎 · 单机入口
// 引擎本体在 core/business/physics/input/render；本文件只做装配：
//   Vulkan 上下文 → 进程内 SPSC 队列 → 四线程启动 → 主线程 GLFW 事件循环 → 优雅停止。
#include "business/BusinessWorld.hpp"
#include "business/PhysicsBridge.hpp"
#include "business/RenderBridge.hpp"
#include "core/Log.hpp"
#include "core/ThreadLoop.hpp"
#include "core/TimeManager.hpp"
#include "input/InputSampler.hpp"
#include "physics/PhysicsWorld.hpp"
#include "render/Renderer.hpp"
#include "render/VulkanContext.hpp"
#include "shared/state/CameraState.hpp"
#include "shared/protocol/Protocol.hpp"
#include "shared/state/PlayerInput.hpp"
#include "registry/BlockRegistry.hpp"
#include "render/assets/ModelLoader.hpp"
#include "worldgen/noise/BuiltinNoiseTypes.hpp"
#include "worldgen/noise/NoiseRegistry.hpp"
#include "worldgen/preset/PresetLoader.hpp"
#include "worldgen/preset/WorldPreset.hpp"
#include "worldgen/generator/WorldGenerator.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <sstream>
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

// 玩家操作说明（输入层常量）：相机控制帮助文本。
// 原定义在 render/VulkanContext（按键输入迁走后的遗留孤岛），按职责归位到引擎入口。
constexpr const char* kCameraHelp = "相机控制：WASD=平移 空格/左Shift=上下 鼠标左键拖动=旋转 R=重置视角\n";

// 把工作目录切到 exe 所在目录：稳定解析 shaders/、textures/ 相对路径。
// 无论从 IDE / 命令行 / 双击启动，资源相对路径都不依赖"进程从哪启动"。
static void chdirToExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0) return;
    std::string exePath(buf, n);
    const auto pos = exePath.find_last_of('\\');
    if (pos == std::string::npos) return;
    SetCurrentDirectoryA(exePath.substr(0, pos).c_str());
#endif
}

// 引擎主流程：装配四轨 → 启动 → 主线程事件循环 → 优雅停止。
static int run() {
    initConsole();
    std::cout << "=== 沙盒引擎 ===\n";

    //方块注册表：必须先于 vkCtx.init（内部 createTextureImage 构建图集要遍历注册表）
    engine::registry::initBlockRegistry();

    //模型注册表：方块注册之后、Vulkan 初始化之前——
    //图集 buildFromRegistry 要收集模型引用的纹理名（如 wood.json 的 log_top）
    engine::assets::loadBlockModels();

    //世界生成子系统（#33/#34）：内置噪声注册 → 行为自检 → 锁表。
    //freeze 后注册表只读；模组 DLL 将来在 freeze 前注册（#41 加载器）。
    engine::worldgen::initBuiltinNoiseTypes();
    const bool noiseOk = engine::worldgen::selfTestNoiseTypes();
    engine::worldgen::NoiseRegistry::freeze();
    std::cout << "噪声注册表：内置 value/perlin/fbm，frozen="
              << std::boolalpha << engine::worldgen::NoiseRegistry::frozen()
              << "，自检" << (noiseOk ? "通过" : "失败") << "\n";

    // 世界预设（#35）：data/worldgen/presets/default.json → WorldPreset（噪声已实例化）。
    engine::worldgen::WorldPreset preset;
    const bool presetOk = engine::worldgen::PresetLoader::load("default", preset);
    std::cout << "世界预设：default.json " << (presetOk ? "加载成功" : "加载失败");
    if (presetOk) {
        std::cout << "（seed=" << preset.seed
                  << " baseHeight=" << preset.baseHeight
                  << " amplitude=" << preset.amplitude
                  << " strata层=" << preset.strata.size()
                  << " 高度图噪声=" << (preset.heightmap ? "已实例化" : "空") << "）";
    }
    std::cout << "\n";

    engine::render::VulkanContext vkCtx;
    const bool vkOk = vkCtx.init(1280, 720, "沙盒引擎 · Vulkan 方块+相机");
    if (vkOk) {
        std::cout << kCameraHelp;
    }

    // 全局 time_scale 单实例，三条轨共享
    core::TimeManager tm;
    // 桥（通信层）：跨线程数据唯一通道，由装配层创建并分发端点。
    //   PhysicsBridge = B1 物理指令（业务→物理）+ B2 回执（物理→业务）+ 指令状态机
    //   RenderBridge  = B3 变更事件（业务→渲染）
    business::PhysicsBridge physicsBridge;
    business::RenderBridge  renderBridge;

    // 玩家输入共享槽（按职责拆分）：
    //   CameraState：相机位置/视角（输入线程写；渲染算 MVP、业务做射线，只读）；
    //   PlayerInput ：点击事件（鼠标回调写；业务消费做 DDA）。
    shared::CameraState cameraState;
    shared::PlayerInput  playerInput;
    vkCtx.attachCameraState(&cameraState);

    business::BusinessWorld businessWorld(tm, physicsBridge, renderBridge);

    //#36 初始世界：WorldGenerator 从预设生成（替换 World 构造里写死的 48×48 平台）。
    // 装配期单线程执行（三轨未启动）；失败不阻断启动（世界保持为空）。
    // 统计外置：下方相机出生点要按实际地表高度设定。
    engine::worldgen::WorldGenerator::Stats genStats;
    bool genOk = false;
    if (presetOk) {
        engine::worldgen::WorldGenerator gen(businessWorld.world(), preset);
        genOk = gen.generate(&genStats);
        std::cout << "地形生成：" << (genOk ? "成功" : "失败");
        if (genOk) {
            std::cout << "（列=" << genStats.columns << " 方块=" << genStats.blocks
                      << " 地表高度=" << genStats.minSurfaceY << ".." << genStats.maxSurfaceY << "）";
        }
        std::cout << "\n";
    }

    physics::PhysicsWorld   physicsWorld(tm);
    input::InputSampler inputSampler;
    inputSampler.attach(vkCtx.window(), &cameraState, &playerInput); // 注册键盘/鼠标回调（真实输入，主线程安全）

    // 相机出生点：按生成地形实际最高地表 +2 格（站在地表上方、略低头看地形）。
    // 不写死世界高度——改预设的 baseHeight/amplitude 后相机会自动跟着走。
    // 生成失败则保持默认出生点（空世界下无意义）。
    if (genOk) {
        inputSampler.setSpawn(0.0,
                              static_cast<double>(genStats.maxSurfaceY) + 2.0,
                              8.0,          // z=8：站在世界中心偏后，朝 -Z 看地形
                              0.0,          // yaw：朝 -Z
                              -0.35);       // pitch：略低头（sin(pitch) 为视线高度分量）
    }

    // 渲染线程（第 4 轨只读观众）：消费变更事件流 + 绘制。
    engine::render::Renderer renderer(businessWorld, renderBridge.changeEventQueue(), vkCtx);

    // 初始世界统计：生成后快照（诊断/验收用）。
    std::cout << "初始世界：方块=" << businessWorld.blockCount() << "\n";

    core::ThreadLoop businessLoop("business");
    core::ThreadLoop physicsLoop("physics");
    core::ThreadLoop inputLoop("input");
    core::ThreadLoop renderLoop("render");

    // ---- 业务线程 25TPS：固定 40ms 墙钟节奏 ----
    businessLoop.start([&] {
        auto next = std::chrono::steady_clock::now();
        while (!businessLoop.shouldStop()) {
            businessWorld.advanceTick(tm.effectiveScale(core::Domain::Business));
            businessWorld.processPlayerInput(cameraState, playerInput);  // 玩家点击 → DDA → 放置/破坏
            businessWorld.syncToPhysics();       // 发指令 → 物理（经 PhysicsBridge）
            businessWorld.receiveFromPhysics();  // 收结果 ← 物理（经 PhysicsBridge）
            businessWorld.flushChanges();        // 变更事件 → 渲染（经 RenderBridge）
            next += std::chrono::milliseconds(40);
            std::this_thread::sleep_until(next);
        }
    });

    // ---- 物理线程 25TPS：accumulator 累积驱动 ----
    physicsLoop.start([&] {
        auto prev = std::chrono::steady_clock::now();
        while (!physicsLoop.shouldStop()) {
            const auto now = std::chrono::steady_clock::now();
            const double wallDt =
                std::chrono::duration<double>(now - prev).count();
            prev = now;
            physicsWorld.advance(tm.effectiveScale(core::Domain::Physics), wallDt,
                                 physicsBridge.commandQueue());
            physicsWorld.syncToBusiness(physicsBridge.resultQueue());
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 避免忙等
        }
    });

    // ---- 输入线程：消费回调增量 → 更新相机 → 写 PlayerInput（~125Hz，墙钟原生）----
    inputLoop.start([&] {
        while (!inputLoop.shouldStop()) {
            inputSampler.sample();
            std::this_thread::sleep_for(std::chrono::milliseconds(8)); // ~125Hz
        }
    });

    // ---- 渲染线程（第 4 轨只读观众，~165Hz）：消费变更事件 + 绘制 ----
    // 渲染不向模拟发任何指令；数据源 = businessToRender 事件流（增量）+ 锁内快照。
    renderLoop.start([&] {
        while (!renderLoop.shouldStop()) {
            renderer.tick();
            vkCtx.drawFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // ~165Hz
        }
    });

    // ---- 主线程：GLFW 事件循环（窗口保持可拖动/缩放响应），窗口关闭 → 优雅停止 ----
    while (!vkCtx.shouldClose()) {
        vkCtx.pollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // 优雅停止：模拟三轨先停，渲染线程最后停（第 4 轨只读，不阻塞模拟停止）。
    businessLoop.stop();
    physicsLoop.stop();
    inputLoop.stop();
    renderLoop.stop();

    vkCtx.shutdown();
    std::cout << "引擎退出。\n";
    return 0;
}

int main() {
    initConsole();
    chdirToExeDir();
    return run();
}

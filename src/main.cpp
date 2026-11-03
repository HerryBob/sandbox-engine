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
#include "modhost/ModLoader.hpp"
#include "render/assets/ModelLoader.hpp"
#include "worldgen/noise/BuiltinNoiseTypes.hpp"
#include "worldgen/noise/NoiseRegistry.hpp"
#include "worldgen/preset/PresetLoader.hpp"
#include "worldgen/preset/WorldPreset.hpp"
#include "worldgen/generator/ColumnGenerator.hpp"
#include "worldgen/streaming/ChunkStreamer.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <iostream>
#include <memory>
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

    //模组加载（#41）：必须夹在 initBlockRegistry 之后、freeze 之前。
    //顺序错了不会有报错，只会静默少方块 —— Registry::add 在表冻结后直接返回 false。
    const auto modCount = engine::modhost::ModLoader::loadAll("mods");
    engine::registry::freezeBlockRegistry();
    std::cout << "方块注册表：共 " << engine::registry::g_blockRegistry.size()
              << " 个（成功加载模组 " << modCount << " 个），frozen="
              << std::boolalpha << engine::registry::g_blockRegistry.frozen() << "\n";

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

    // ---- 1b 流式生成（#38）----
    // 线程边界：业务线程是 World 唯一写者；后台生成线程只做纯计算（ColumnGenerator），
    //           不持有 World、不碰方块存储（8.4 条准则"线程跟着数据是否共享走"）。
    // 装配期先同步预热出生点窗口（9×9 = 81 列），再交给后台线程按玩家位置持续请求。
    engine::worldgen::ColumnGenerator columnGen(preset);
    bool genOk = false;
    std::unique_ptr<engine::worldgen::ChunkStreamer> streamer;

    if (presetOk && columnGen.prepare()) {
        genOk = true;
        const int radius = engine::worldgen::ChunkStreamer::kDefaultRadius;
        const int span = 2 * radius + 1;
        const std::size_t warmupColumns = static_cast<std::size_t>(span * span);   // 81

        streamer = std::make_unique<engine::worldgen::ChunkStreamer>(columnGen, radius);
        streamer->start();

        // 同步预热：阻塞到窗口内 81 列全部写入（装配期没有 25TPS 约束）。
        // 刻意走和运行期完全同一条路径（tickUpdate → 后台生成 → drainResults → World），
        // 避免"启动一套代码、运行期另一套代码"，预热与动态加载结果必然一致。
        auto& world = businessWorld.world();
        const double spawnX = 0.0;
        const double spawnZ = 8.0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (streamer->stats().applied < warmupColumns &&
               std::chrono::steady_clock::now() < deadline) {
            streamer->tickUpdate(spawnX, spawnZ);
            streamer->drainResults(world, engine::worldgen::ChunkStreamer::kMaxApplyPerTick);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // 首帧渲染走全量快照重建：装配期的脏标记清掉，避免首帧后再重复重建一遍
        world.clearDirtyChunks();

        const auto s = streamer->stats();
        std::cout << "流式预热：请求列=" << s.requested
                  << " 已生成=" << s.generated
                  << " 已应用=" << s.applied
                  << " 方块=" << s.blocks
                  << " 地表=" << s.minSurfaceY << ".." << s.maxSurfaceY
                  << " chunk=" << world.chunkCount() << "(非空，空chunk不建)";
        if (s.applied < warmupColumns) std::cout << " [未灌满：30s 超时]";
        std::cout << "\n";
    } else {
        std::cout << "流式预热：失败（预设未加载或地层校验未通过）\n";
    }

    physics::PhysicsWorld   physicsWorld(tm);
    input::InputSampler inputSampler;
    inputSampler.attach(vkCtx.window(), &cameraState, &playerInput); // 注册键盘/鼠标回调（真实输入，主线程安全）

    // 相机出生点：按预热窗口实际最高地表 +2 格（1b 决策点③：用实测值，不写死世界高度）。
    // 不写死的好处：改预设的 baseHeight/amplitude 后相机会自动跟着走。
    // 预热失败则保持默认出生点（空世界下无意义）。
    if (genOk && streamer) {
        const auto s = streamer->stats();
        inputSampler.setSpawn(0.0,
                              static_cast<double>(s.maxSurfaceY) + 2.0,
                              8.0,          // z=8：站在世界中心偏后，朝 -Z 看地形
                              0.0,          // yaw：朝 -Z
                              -0.35);       // pitch：略低头（sin(pitch) 为视线高度分量）
    }

    // 渲染线程（第 4 轨只读观众）：消费变更事件流 + 绘制。
    engine::render::Renderer renderer(businessWorld, renderBridge.changeEventQueue(), vkCtx, playerInput);

    // 初始世界统计：生成后快照（诊断/验收用）。
    std::cout << "初始世界：方块=" << businessWorld.blockCount() << "\n";

    core::ThreadLoop businessLoop("business");
    core::ThreadLoop physicsLoop("physics");
    core::ThreadLoop inputLoop("input");
    core::ThreadLoop renderLoop("render");

    // ---- 业务线程 25TPS：固定 40ms 墙钟节奏 ----
    // 流式统计上报节流：每 25 个业务 tick（约 1 秒）打一行。
    // 读 stats() 只能在业务线程做——appliedBlocks/minSurface 等聚合字段由 drainResults 独占写。
    std::size_t streamReportCounter = 0;
    businessLoop.start([&] {
        auto next = std::chrono::steady_clock::now();
        while (!businessLoop.shouldStop()) {
            businessWorld.advanceTick(tm.effectiveScale(core::Domain::Business));
            businessWorld.processPlayerInput(cameraState, playerInput);  // 玩家点击 → DDA → 放置/破坏
            // 1b 流式：请求窗口内未加载的列 + 应用后台已算好的结果。
            // 必须在 flushChanges 之前——新到货 chunk 的脏标记要进本 tick 的变更事件，
            // 否则渲染端要等下一个 tick 才看到它。
            if (streamer) {
                streamer->tickUpdate(cameraState.camX.load(), cameraState.camZ.load());
                streamer->drainResults(businessWorld.world(),
                                       engine::worldgen::ChunkStreamer::kMaxApplyPerTick);
                if (++streamReportCounter >= 25) {
                    streamReportCounter = 0;
                    const auto s = streamer->stats();
                    std::cout << "[流式] 请求=" << s.requested << " 生成=" << s.generated
                              << " 应用=" << s.applied << " 方块=" << s.blocks
                              << " 地表=" << s.minSurfaceY << ".." << s.maxSurfaceY
                              << " 世界chunk=" << businessWorld.world().chunkCount() << "\n";
                }
            }
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
            // 这里原本有一行 sleep_for(5ms) 做限速，已删除：节奏由 FIFO(vsync) 的
            // vkAcquireNextImageKHR 阻塞决定，帧率上限 = 显示器刷新率；
            // 且 Windows 定时器粒度会把 5ms 请求放大到约 9.5ms（实测 165→62 FPS）。
        }
    });

    // ---- 主线程：GLFW 事件循环（窗口保持可拖动/缩放响应），窗口关闭 → 优雅停止 ----
    while (!vkCtx.shouldClose()) {
        vkCtx.pollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // 优雅停止：模拟三轨先停，渲染线程最后停（第 4 轨只读，不阻塞模拟停止）。
    businessLoop.stop();
    if (streamer) streamer->stop();   // 业务线程已停（不再 drainResults），再停生成线程
    physicsLoop.stop();
    inputLoop.stop();
    renderLoop.stop();

    vkCtx.shutdown();
    //模组逆序卸载：放在渲染/业务全部停掉之后——
    //注册表里还存着 DLL 提供的字符串指针，提前卸载会变成悬垂引用。
    engine::modhost::ModLoader::unloadAll();
    std::cout << "引擎退出。\n";
    return 0;
}

int main() {
    initConsole();
    chdirToExeDir();
    return run();
}

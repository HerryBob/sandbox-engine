// 沙盒引擎 · 单机入口
// 引擎本体在 core/business/physics/input/render；本文件只做装配：
//   Vulkan 上下文 → 进程内 SPSC 队列 → 三线程启动（业务/物理/渲染）→ 主线程 GLFW 事件循环 → 优雅停止。
#include "core/SelfTest.hpp"
#include "business/BusinessWorld.hpp"
#include "business/PhysicsBridge.hpp"
#include "business/RenderBridge.hpp"
#include "core/Log.hpp"
#include "core/ThreadLoop.hpp"
#include <vector>

#include "core/TimeManager.hpp"
#include "shared/state/PoseHistory.hpp"
#include "input/InputSampler.hpp"
#include "physics/JoltKernel.hpp"
#include "physics/PhysicsWorld.hpp"
#include "render/Renderer.hpp"
#include "render/VulkanContext.hpp"
#include "render/VulkanMesh.hpp"
#include "shared/chunk/ChunkData.hpp"
#include "shared/state/CameraState.hpp"
#include "shared/state/PlayerBodyState.hpp"
#include "shared/state/PlayerControlState.hpp"
#include "shared/protocol/Protocol.hpp"
#include "shared/state/PlayerInput.hpp"
#include "registry/BlockRegistry.hpp"
#include "modhost/entity/EntityDefTable.hpp"
#include "modhost/entity/EntityInstanceTable.hpp"
#include "modhost/loader/ModLoader.hpp"
#include "modhost/manifest/Manifest.hpp"
#include "modhost/sorter/ModSorter.hpp"
#include "resource/ResourceIndex.hpp"
#include "render/assets/ModelDef.hpp"
#include "render/assets/ModelLoader.hpp"
#include "render/assets/EntityModel.hpp"
#include "render/assets/BbModelLoader.hpp"
#include "worldgen/noise/BuiltinNoiseTypes.hpp"
#include "worldgen/noise/NoiseDefTable.hpp"
#include "worldgen/noise/NoiseRegistry.hpp"
#include "worldgen/preset/PresetLoader.hpp"
#include "worldgen/preset/WorldPreset.hpp"
#include "worldgen/generator/ColumnGenerator.hpp"
#include "worldgen/streaming/ChunkStreamer.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
constexpr const char* kCameraHelp =
    "相机控制：WASD=平移 空格/左Shift=上下 移动鼠标=转视角 R=重置视角\n"
    "         F4=自由相机/玩家跟随切换 F5=第一人称/第三人称切换（仅玩家模式） F3=调试浮层\n";

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

// 引擎主流程：装配三条轨 → 启动 → 主线程事件循环 → 优雅停止。
// presetName：世界预设名（不带 .json），由启动参数 --preset 决定。
// ============================================================================
// run() 的六个自包含段（规则1 拆分）。
// run() 本身是装配根：绝大多数局部变量被后续阶段使用，整体参数化需要引入
// AppContext（超出"只拆分"的范围），所以这里只抽"不产生后续依赖"的阶段。
// ============================================================================

// 模组加载 → 冻结方块/实体注册表 → 解析内置 id → 打印概况 → 加载方块模型。
// 顺序有硬约束：整条链必须早于 vkCtx.init（图集要遍历方块注册表）。
static void loadModsAndFreezeRegistries() {
    //方块注册表（#39）：引擎不再写死任何方块，内容全部由 mods/blocks.dll 注册。
    //顺序：loadAll（blocks.dll 按文件名排序最靠前，独占 id 1..6）→ freeze → 按名字解析内置 id。
    //晚于 freeze 的注册会被 Registry::add 静默拒绝；整条链必须早于 vkCtx.init（图集要遍历注册表）。
    const auto modCount = engine::modhost::ModLoader::loadAll("mods");
    engine::registry::freezeBlockRegistry();
    engine::registry::resolveBuiltinBlockIds();
    // 实体类型表：与方块表同一窗口封表。模组的 onLoad 只能登记描述，
    // 真正的模型/贴图加载放到 vkCtx.init 之后（那时 Vulkan 才就绪）。
    engine::modhost::freezeEntityTypeRegistry();
    engine::modhost::reserveEntityInstanceIdZero();
    std::cout << "实体类型注册表：共 " << engine::modhost::g_entityTypeRegistry.size()
              << " 个类型，frozen="
              << std::boolalpha << engine::modhost::g_entityTypeRegistry.frozen() << "\n";
    std::cout << "方块注册表：共 " << engine::registry::g_blockRegistry.size()
              << " 个（成功加载模组 " << modCount << " 个），frozen="
              << std::boolalpha << engine::registry::g_blockRegistry.frozen() << "\n";
    std::cout << "资源索引：assets "
              << resource::g_resourceIndex.count(resource::ResourceKind::Assets)
              << " 个文件，data "
              << resource::g_resourceIndex.count(resource::ResourceKind::Data)
              << " 个文件\n";

    //模型注册表：方块注册之后、Vulkan 初始化之前——
    //图集 build 要遍历模型表的纹理槽位补纹理名，所以模型必须先烘焙好
    engine::assets::loadBlockModels();
}

// 噪声注册（模组批 + 内置 flat）→ 六项自检 → 打印。结果只打印，不影响启动流程。
static void initNoiseAndRunSelfTests() {
    //世界生成子系统（#33/#34）：噪声注册 → 行为自检 → 锁表。
    //注册分两批，顺序固定：
    //  ① 模组（上面 loadAll 已调完，各 DLL 在 onLoad 里 registerNoise）
    //  ② 引擎内置 flat（自举项：删掉所有 DLL 后，只剩它仍能跑平地）
    //freeze 之后只读；晚到的注册会被 NoiseRegistry::registerType 直接拒绝。
    engine::worldgen::initBuiltinNoiseTypes();
    const bool noiseOk = engine::worldgen::selfTestNoiseTypes();
    engine::worldgen::NoiseRegistry::freeze();

    //区块调色板（ChunkData）：坐标编解码互逆 + 4/8/16 位扩宽 + 判空自检，装配期跑一次。
    const bool paletteOk = engine::selftest::palette();

    //元素级旋转（ModelElement::rotation）：绕轴旋转公式自检。
    const bool modelRotOk = engine::selftest::modelRotation();

    //元素级旋转的几何落点：rotation 字段 → buildModelVertices 顶点位置。
    const bool modelGeoOk = engine::selftest::modelGeometry();

    //刚体网格（可动方块组）：3×3×3 内部剔除 → 54 面 / 216 顶点。
    const bool bodyMeshOk = engine::selftest::bodyMesh();

    //世界 chunk 网格（ChunkMeshBuilder，纯 CPU）：空输入 / 单方块 6 面 / 3×3×3 实心 54 面。
    //这是渲染层拆分后的第一个可单测点（不需要窗口与 GPU）。
    const bool chunkMeshOk = engine::selftest::chunkMesh();

    //刚体物理内核（Jolt）：初始化 + 地面/箱子跑 100 步 → 箱子落在地面顶面上。
    //验的是"内核能不能起来"（编译/链接/初始化顺序/分配器/精度），不碰引擎物理与渲染。
    const bool joltKernelOk = engine::physics::selfTestJoltKernel();

    //模组排序（ModSorter）：跨批次依赖方向、缺失依赖、同批环。
    const bool sorterOk = engine::selftest::modSorter();

    //世界生成数据（data/worldgen/noise/*.json）：扫全目录、逐个解码注册，之后按名字查表。
    //必须在预设加载之前——PresetLoader 只查这张表，不再自己读噪声文件。
    engine::worldgen::loadNoiseDefs();

    // 按名字区分来源：flat 是内置，其余全部来自模组 DLL（注册表不记来源元数据）。
    std::vector<std::string> noiseTypes = engine::worldgen::NoiseRegistry::types();
    std::sort(noiseTypes.begin(), noiseTypes.end());
    std::string modNoise;
    for (const std::string& type : noiseTypes) {
        if (type == "flat") continue;
        if (!modNoise.empty()) modNoise += "/";
        modNoise += type;
    }
    const std::size_t modNoiseCount =
        noiseTypes.size() - (engine::worldgen::NoiseRegistry::contains("flat") ? 1U : 0U);
    std::cout << "噪声注册表：内置 flat，模组注册 " << modNoiseCount
              << " 个（" << modNoise << "），frozen="
              << std::boolalpha << engine::worldgen::NoiseRegistry::frozen()
              << "，自检" << (noiseOk ? "通过" : "失败") << "\n";
    std::cout << "区块调色板：坐标互逆 + 4/8/16 位扩宽 + 判空自检"
              << (paletteOk ? "通过" : "失败") << "\n";
    std::cout << "模型元素旋转：绕轴旋转自检"
              << (modelRotOk ? "通过" : "失败") << "\n";
    std::cout << "模型元素旋转：顶点几何自检"
              << (modelGeoOk ? "通过" : "失败") << "\n";
    std::cout << "刚体网格：3×3×3 内部剔除自检"
              << (bodyMeshOk ? "通过" : "失败") << "\n";
    std::cout << "chunk 网格：空/单块/3×3×3 内部剔除自检"
              << (chunkMeshOk ? "通过" : "失败") << "\n";
    std::cout << "物理内核：Jolt 初始化 + 地面/箱子 100 步落地自检"
              << (joltKernelOk ? "通过" : "失败") << "\n";
    std::cout << "模组排序：跨批次依赖方向 + 缺失依赖 + 同批环自检"
              << (sorterOk ? "通过" : "失败") << "\n";
}

// 把一批流式结果落地到世界（业务线程 / 装配期单线程调用）。
// 规则5：ChunkStreamer 只产数据（它不持有 World），"写 World"留在唯一写者这一侧。
static void applyStreamBatch(engine::business::World& world,
                             const engine::worldgen::ChunkStreamer::StreamBatch& batch) {
    if (!batch.deadChunks.empty()) world.unloadChunks(batch.deadChunks);
    for (const auto& col : batch.columns) {
        const auto& blocks = *col.blocks;
        for (int level = 0; level < engine::worldgen::kGenChunkLevels; ++level) {
            const engine::shared::ChunkPos cp{
                col.pos.x,
                (engine::worldgen::kGenMinY / engine::shared::CHUNK_SIZE) + level,
                col.pos.z};
            world.applyGeneratedChunk(cp, blocks.chunks[level]);
        }
    }
}

// 同步预热：阻塞到窗口内 warmupColumns 列全部写入（装配期没有 25TPS 约束）。
// 刻意走和运行期完全同一条路径（tick → 后台生成 → takeBatch → World）。
static void warmupWindow(engine::worldgen::ChunkStreamer& streamer,
                         engine::business::World& world,
                         std::size_t warmupColumns) {
    const double spawnX = 0.0;
    const double spawnZ = 8.0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (streamer.stats().applied < warmupColumns &&
           std::chrono::steady_clock::now() < deadline) {
        streamer.tick(spawnX, spawnZ);
        applyStreamBatch(world, streamer.takeBatch(engine::worldgen::ChunkStreamer::kMaxApplyPerTick));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto s = streamer.stats();
    std::cout << "流式预热：请求列=" << s.requested
              << " 已生成=" << s.generated
              << " 已应用=" << s.applied
              << " 方块=" << s.blocks
              << " 地表=" << s.minSurfaceY << ".." << s.maxSurfaceY
              << " chunk=" << world.chunkCount() << "(非空，空chunk不建)";
    if (s.applied < warmupColumns) {
        // B123：预热失败**不致命**（硬失败会让慢机器/慢磁盘直接起不来，按决定不做），
        // 但后果必须可见：世界只有部分列，渲染端可能一个 chunk 都没有，
        // 后续所有"看不见东西/碰撞对不上"的现象都可能源于此。
        ENGINE_LOG_ERROR("Main",
            "流式预热超时：已应用 " + std::to_string(s.applied) + "/" +
            std::to_string(warmupColumns) + " 列 —— 世界不完整，渲染端可能没有 chunk；"
            "继续启动（按设计不硬失败）");
        std::cout << " [未灌满：30s 超时]";
    }
    std::cout << "\n";
}

// 区块调色板统计（诊断/验收用）：实际占用 vs 16 位密集数组的对比。
static void printPaletteStats(engine::business::World& world) {
    const auto ps = world.paletteStats();
    const std::size_t denseBytes = ps.chunks * 8192;   // 改造前：16 位密集数组
    std::cout << "[Palette] chunk=" << ps.chunks
              << " 4位=" << ps.bits4 << " 8位=" << ps.bits8 << " 16位=" << ps.bits16
              << " 平均字典=" << std::fixed << std::setprecision(2) << ps.avgPalette << "项"
              << " 实际=" << ps.totalBytes / 1024 << "KB"
              << " 密集数组本应=" << denseBytes / 1024 << "KB"
              << "（省 " << (denseBytes == 0 ? 0.0
                      : static_cast<double>(denseBytes - ps.totalBytes) * 100.0
                        / static_cast<double>(denseBytes)) << "%）\n";
}

// 加玩家物理体：碰撞箱半宽/半高/半深与跳跃初速取自实体类型定义（缺失时退回本项目默认 0.8×1.8×0.8、跳速 4.9）。
static void addPlayerPhysicsBody(engine::physics::PhysicsWorld& physics,
                                 const engine::modhost::EntityTypeInfo* playerType) {
    const double halfX = (playerType ? playerType->width : 0.8) * 0.5;
    const double halfY = (playerType ? playerType->height : 1.8) * 0.5;
    const double halfZ = (playerType ? playerType->depth : 0.8) * 0.5;
    const double jump  = (playerType ? playerType->jumpSpeed : 4.9);
    physics.addPlayerBody(0.0, 8.0, 0.0, halfX, halfY, halfZ, jump);
    std::cout << "玩家碰撞箱：半宽=" << halfX << " 半高=" << halfY << " 半深=" << halfZ
              << " 跳跃初速=" << jump << "\n";
}

// 帧门闩（frame gate 帧门闩）：把主线程的 GLFW 事件泵与渲染帧串成"一次 poll 对应一帧"。
// 时序：主线程等上一帧收尾 → pollEvents（回调只写原子）→ 置 pollReady；
//       渲染线程取走 pollReady → 渲染整帧 → 置 frameReady。
// stopped 置真后所有等待立即返回，两边都不再互相等。
struct FrameGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool pollReady = false;    // 有一次 poll 尚未被渲染帧取走
    bool frameReady = true;    // 上一帧已收尾（初始 true：首轮 poll 不必等）
    bool stopped = false;      // 已关闭，等待者应退出

    // 主线程：等上一帧收尾并占位。返回 false 表示已关闭，调用方应退出循环。
    // 超时只是防"渲染线程卡死导致主线程再也无法 poll"的死锁兜底，正常一帧约 6ms。
    bool waitFrameConsumed() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(100),
                    [this] { return frameReady || stopped; });
        if (stopped) return false;
        frameReady = false;
        return true;
    }

    // 主线程：本次 poll 完成，交给渲染线程消费。
    void markPolled() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            pollReady = true;
        }
        cv.notify_all();
    }

    // 渲染线程：取走一次 poll。返回 false 表示已关闭，调用方应退出循环。
    bool waitPoll() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return pollReady || stopped; });
        if (stopped) return false;
        pollReady = false;
        return true;
    }

    // 渲染线程：本帧收尾，通知主线程可以下一次 poll。
    void markFrameDone() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            frameReady = true;
        }
        cv.notify_all();
    }

    // 关闩：唤醒所有等待者，此后 wait* 一律返回 false。
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
        }
        cv.notify_all();
    }
};

static int run(const std::string& presetName) {
    initConsole();
    std::cout << "=== 沙盒引擎 ===\n";

    // 资源索引：先登记引擎自带的 assets/ 与 data/，再由 ModLoader 追加各模组目录下的同名子目录。
    // 登记顺序即冲突优先级：先登记者优先。
    resource::g_resourceIndex.addRoot(resource::ResourceKind::Assets, "", "assets");
    resource::g_resourceIndex.addRoot(resource::ResourceKind::Data, "", "data");

    loadModsAndFreezeRegistries();

    initNoiseAndRunSelfTests();

    // 世界预设（#35）：data/worldgen/presets/<presetName>.json → WorldPreset（纯数据）。
    // 高度场实例化推迟到 ColumnGenerator::prepare()，失败原因在那里报。
    engine::worldgen::WorldPreset preset;
    const bool presetOk = engine::worldgen::PresetLoader::load(presetName, preset);
    std::cout << "世界预设：" << presetName << ".json " << (presetOk ? "加载成功" : "加载失败");
    if (presetOk) {
        std::cout << "（seed=" << preset.seed
                  << " baseHeight=" << preset.baseHeight
                  << " amplitude=" << preset.amplitude
                  << " strata层=" << preset.strata.size()
                  << " 高度场=" << preset.heightType << "）";
    }
    std::cout << "\n";
    if (!presetOk) {
        // 没有预设就没有地形来源：继续启动只会得到空世界，直接退出。
        std::cerr << "预设加载失败，没有可生成的世界，退出。\n";
        return 1;
    }

    engine::render::VulkanContext vkCtx;
    const bool vkOk = vkCtx.init(1280, 720, "沙盒引擎 · Vulkan 方块+相机");

    // 玩家实体 id / 类型定义：类型表里按名字找，模型、贴图、体型数值全由模组定义提供。
    // id = -1 表示没找到（模组没加载），后续位姿/可见性写入全部跳过。
    // playerType 供后面的物理与输入注入读数值，因此提到外层作用域。
    std::int32_t playerEntityId = -1;
    const engine::modhost::EntityTypeInfo* playerType = nullptr;
    if (vkOk) {
        std::cout << kCameraHelp;

        // ---- 实体外观：类型表 → 实例 → GPU 模型 ----
        // 模型/贴图路径已不在本文件写死，全部来自模组注册的类型定义。
        // 此刻渲染线程尚未启动，VkBuffer 创建是安全的。
        const std::int32_t playerTypeId = engine::modhost::findEntityTypeId("player");
        if (playerTypeId >= 0) {
            playerType = engine::modhost::g_entityTypeRegistry.find(playerTypeId);
            if (playerType) {
                playerEntityId = engine::modhost::createEntityInstance(playerTypeId);
                if (!vkCtx.loadEntityModel(playerEntityId, playerType->model, playerType->texture)) {
                    std::cerr << "玩家模型加载失败（类型=" << playerType->name
                              << " 模型=" << playerType->model << "），继续运行但没有玩家外观\n";
                } else {
                    std::cout << "玩家实体：id=" << playerEntityId
                              << " 类型=" << playerType->name << "（id=" << playerTypeId << "）"
                              << " 模型=" << playerType->model << "\n";
                }
            }
        } else {
            std::cerr << "未找到名为 player 的实体类型（模组未加载？），玩家不可见\n";
        }
    }

    // 全局 time_scale 单实例，业务/物理两条轨共享（渲染轨不参与模拟节奏）
    core::TimeManager tm;

    // 跨线程实体位姿表（玩家 slot 0 + 刚体按 structureId 分配）：物理每 tick 发布一帧，
    // 渲染每帧按 renderTick 查表插值。替代原来的 物理→结果队列→业务→渲染队列 两跳，
    // 让刚体延迟从 80ms 降到 40ms，并与玩家共用同一套插值。
    // 6 帧环 × 2048 槽（每帧 64KB）+ id→slot 索引表，合计约 450KB，堆上分配。
    auto poseHistory = std::make_unique<engine::shared::PoseHistory>();
    // 桥（通信层）：跨线程数据唯一通道，由装配层创建并分发端点。
    //   PhysicsBridge = B1 物理指令（业务→物理）+ B2 回执（物理→业务）+ 指令状态机
    //   RenderBridge  = B3 变更事件（业务→渲染）
    business::PhysicsBridge physicsBridge;
    business::RenderBridge  renderBridge;

    // 玩家输入共享槽（按职责拆分）：
    //   CameraState：相机位置/视角（渲染线程写；渲染算 MVP、业务做射线，只读）；
    //   PlayerInput ：点击事件（鼠标回调写；业务消费做 DDA）。
    shared::CameraState cameraState;
    shared::PlayerInput  playerInput;
    // 玩家体位置（物理线程写 → 渲染线程读）：F4 切到玩家模式后相机跟随它。
    shared::PlayerBodyState playerBodyState;
    // 玩家操控意图（渲染线程写 → 物理线程读）：玩家模式下 WASD/空格驱动玩家体。
    shared::PlayerControlState playerControlState;
    vkCtx.attachCameraState(&cameraState);

    // ---- 刚体（第 4 步）：由业务线程打包驱动、渲染线程消费 ----
    // 方块列表不再在这一侧手工塞入：物理打包回执带刚体局部坐标，
    // 经 PhysicsBridge → BusinessWorld → RenderBridge → 渲染线程建网格。
    business::BusinessWorld businessWorld(tm, physicsBridge, renderBridge);

    // 世界已构造：交给模组 API 层。loadAll 执行时 World 还不存在，
    // 那段窗口里模组调世界查询会拿到 kWorldNotReady。
    engine::modhost::ModLoader::attachWorld(&businessWorld.world());

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

        warmupWindow(*streamer, businessWorld.world(), warmupColumns);
        // 首帧渲染走全量快照重建：装配期的脏标记清掉，避免首帧后再重复重建一遍
        businessWorld.world().clearDirtyChunks();
    } else {
        std::cout << "流式预热：失败（预设未加载或地层校验未通过）\n";
    }

    physics::PhysicsWorld   physicsWorld(tm);
    // 首个生物：玩家（运动学、单 AABB、位置锚在脚底中心），从空中落下验证重力与落地。
    // 碰撞箱半宽/半高/半深与跳跃初速全部取自实体类型定义（模组没加载时退回 MC 口径默认）。
    // 物理线程尚未启动，这里属于装配期单线程，可安全增删物理体。
    addPlayerPhysicsBody(physicsWorld, playerType);
    // 玩家位置共享槽（物理线程写 → 渲染线程读）：相机跟随的唯一数据来源。
    physicsWorld.attachPlayerState(&playerBodyState);
    // 玩家操控槽（渲染线程写 → 物理线程读）：WASD 水平速度 + 空格跳跃。
    physicsWorld.attachPlayerControl(&playerControlState);
    input::InputSampler inputSampler;
    // 眼高与步速同样来自实体类型定义（第一人称相机位置、WASD 速度都读它们）。
    inputSampler.setBodyMetrics(playerType ? playerType->eyeHeight : 1.62,
                                playerType ? playerType->moveSpeed : 4.3);
    // 注册键盘/鼠标回调（真实输入，主线程安全）：F4 切换自由相机 / 玩家跟随。
    inputSampler.attach(vkCtx.window(), &cameraState, &playerInput,
                        &playerBodyState, &playerControlState);

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

    // 渲染线程（第 3 轨只读观众）：消费变更事件流 + 绘制（输入采样也在本轨）。
    engine::render::Renderer renderer(businessWorld, renderBridge.businessToRenderChanges(), vkCtx, playerInput);

    // 初始世界统计：生成后快照（诊断/验收用）。
    std::cout << "初始世界：方块=" << businessWorld.blockCount() << "\n";
    printPaletteStats(businessWorld.world());

    FrameGate frameGate;   // 主线程事件泵 ↔ 渲染帧的"一进一出"门闩
    core::ThreadLoop businessLoop("business");
    core::ThreadLoop physicsLoop("physics");
    core::ThreadLoop renderLoop("render");

    // ---- 业务线程 25TPS：固定 40ms 墙钟节奏 ----
    // 流式统计上报节流：每 25 个业务 tick（约 1 秒）打一行。
    // 读 stats() 只能在业务线程做——appliedBlocks/minSurface 等聚合字段由 receiveResults 独占写。
    std::size_t streamReportCounter = 0;
    bool demoPackQueued = false;   // 演示打包只发一次（业务线程独占，无需原子）
    businessLoop.start([&] {
        auto next = std::chrono::steady_clock::now();
        while (!businessLoop.shouldStop()) {
            businessWorld.tick(tm.scale());
            businessWorld.handlePlayerInput(cameraState, playerInput);  // 玩家点击 → DDA → 放置/破坏
            // 1b 流式：请求窗口内未加载的列 + 应用后台已算好的结果。
            // 必须在 flushChangesToRender 之前——新到货 chunk 的脏标记要进本 tick 的变更事件，
            // 否则渲染端要等下一个 tick 才看到它。
            if (streamer) {
                streamer->tick(cameraState.camX.load(), cameraState.camZ.load());
                const auto batch = streamer->takeBatch(
                    engine::worldgen::ChunkStreamer::kMaxApplyPerTick);
                // 世界 chunk 集合有变化（加载或卸载）→ 物理侧必须重同步，
                // 否则会停在旧的中间态快照（实测：世界 90 列、物理仍 81 列）。
                // 策略按变化数量分流：>8 个走全量重建（下方 requestPhysicsResync，一次建完），
                // 少量走增量、只发这批 chunk 的新内容。稳态 batch 为空，不触发。
                if (!batch.empty()) {
                    applyStreamBatch(businessWorld.world(), batch);
                    // 增量同步：只发这批 chunk 的新内容（不再全量重发 8 万方块）。
                    std::unordered_set<engine::shared::ChunkPos> changed;
                    for (const auto& cp : batch.deadChunks) changed.insert(cp);
                    for (const auto& col : batch.columns) {
                        changed.insert(engine::shared::ChunkPos{col.pos.x, 0, col.pos.z});
                    }
                    if (changed.size() > 8) {
                        // 大批量（启动预热 / 世界重建）：走全量，一次建完。
                        // 走增量的话"每 tick 只重建 1 个 chunk"要排 3 秒以上，
                        // 那段时间物理世界里几乎没有碰撞体（实测 tps 掉到 8）。
                        businessWorld.requestPhysicsResync();
                    } else {
                        std::vector<engine::shared::ChunkPos> changedVec(changed.begin(), changed.end());
                        auto blocks = businessWorld.world().chunkSnapshot(
                            changed, /*expandNeighbors=*/false);
                        businessWorld.syncChunksToPhysics(std::move(blocks), std::move(changedVec));
                    }
                }
                if (++streamReportCounter >= 25) {
                    streamReportCounter = 0;
                    const auto s = streamer->stats();
                    std::cout << "[流式] 请求=" << s.requested << " 生成=" << s.generated
                              << " 应用=" << s.applied << " 方块=" << s.blocks
                              << " 地表=" << s.minSurfaceY << ".." << s.maxSurfaceY
                              << " 世界chunk=" << businessWorld.world().chunkCount() << "\n";
                }
            }
            businessWorld.syncWorldToPhysics();       // 发指令 → 物理（经 PhysicsBridge）
            businessWorld.receiveResultsFromPhysics();  // 收结果 ← 物理（经 PhysicsBridge）
            businessWorld.flushChangesToRender();        // 变更事件 → 渲染（经 RenderBridge）

            // 演示刚体（第 4 步）：世界出第一列后打包一组 3×3×3 石头并给水平初速度，
            // 物理线程积分位置，渲染端跟着移动。方块不经 World——本步只验"物理→渲染"链路。
            if (!demoPackQueued && businessWorld.chunkCount() > 0) {
                demoPackQueued = true;
                engine::shared::PhysicsCommand pack;
                pack.type = engine::shared::PhysicsCommand::Type::PackContraption;
                pack.contraptionId = 1;
                pack.ivx = 1.2; pack.ivy = 0.8; pack.ivz = 0.0; // 起飞速度：+X 前进、+Y 微升
                const std::uint16_t stoneId = engine::registry::g_builtinBlockIds.stone;
                for (int x = -1; x <= 1; ++x)
                    for (int y = 5; y <= 7; ++y)
                        for (int z = -3; z <= -1; ++z)
                            pack.blocks.push_back(engine::shared::Block{
                                static_cast<engine::shared::BlockType>(stoneId),
                                engine::shared::BlockPos{x, y, z}, -1}); // -1 = 主世界坐标
                pack.blockCount = static_cast<int>(pack.blocks.size());
                if (physicsBridge.businessToPhysicsCommands().push(pack))
                    std::cout << "演示刚体：打包指令已发 方块=" << pack.blockCount << "\n";
                else
                    std::cout << "演示刚体：指令队列满，未发出\n";
            }

            next += std::chrono::milliseconds(40);
            std::this_thread::sleep_until(next);
        }
    });

    // ---- 物理线程 25TPS：accumulator 累积驱动 ----
    physicsWorld.attachPoseHistory(poseHistory.get());
    physicsLoop.start([&] {
        auto prev = std::chrono::steady_clock::now();
        while (!physicsLoop.shouldStop()) {
            const auto now = std::chrono::steady_clock::now();
            const double wallDt =
                std::chrono::duration<double>(now - prev).count();
            prev = now;
            physicsWorld.tick(tm.scale(), wallDt,
                                 physicsBridge.businessToPhysicsCommands());
            physicsWorld.pushResultsToBusiness(physicsBridge.physicsToBusinessResults());
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 避免忙等
        }
    });

    // 输入不再单独开线程：鼠标增量由 GLFW 回调累积，**每渲染帧消费一次**（MC 同款）。
    // 之前是 125Hz 的输入线程，而渲染/实体是 ~165Hz —— 相机与实体的运动步长不同，
    // 转身时世界会以 125Hz 步进（实测：把相机钉死后实体本身完全顺滑）。
    // 见渲染循环里的 inputSampler.tick()。

    // ---- 渲染线程（第 3 轨只读观众，~165Hz）：消费变更事件 + 绘制 + 输入采样 ----
    // 渲染不向模拟发任何指令；数据源 = businessToRender 事件流（增量）+ 锁内快照。
    // 渲染端的刚体 id 列表（结构事件增删）；每帧据此查 PoseHistory
    std::vector<int> bodyIds_;
    renderLoop.start([&] {
        while (!renderLoop.shouldStop()) {
            // 等主线程为这一帧完成的事件泵：本帧消费的鼠标/键盘增量恰好是
            // 上一帧到这一帧之间的那一份，不再出现"某帧增量为零、下一帧双倍"。
            // 门闩已关闭 → 直接退出（不再渲染）。
            if (!frameGate.waitPoll()) break;

            renderer.tick();

            // 输入：**每渲染帧消费一次**鼠标/键盘增量（MC 同款）。
            // 放在 renderer.tick 之后、取实体位姿之前 —— 相机与实体用同一帧的插值结果。
            inputSampler.tick();

            // 刚体结构事件（打包建网格 / 解包销毁）：事件流，逐条处理。
            engine::shared::BodyStructureEvent bodyEv;
            while (renderBridge.businessToRenderStructure().pop(bodyEv)) {
                if (bodyEv.destroy) {
                    vkCtx.destroyBodyMesh(bodyEv.contraptionId);
                    for (auto& id : bodyIds_) { if (id == bodyEv.contraptionId) id = -1; }
                } else if (!vkCtx.uploadBodyMesh(bodyEv.contraptionId, bodyEv.blocks)) {
                    std::cout << "刚体网格上传失败 id=" << bodyEv.contraptionId << "\n";
                } else {
                    bodyIds_.push_back(bodyEv.contraptionId);
                }
            }

            // 刚体位姿：**直读跨线程位姿表**（PoseHistory）—— 与玩家同一条模拟时间轴、
            // 同一个 renderTick。原来走 物理→结果队列→业务线程→渲染队列 两跳，
            // 多 40ms 延迟且到达抖动大，现在这条路已绕开。
            {
                const double rt = playerBodyState.renderTickF();
                engine::shared::PoseEntry pe;
                for (const int id : bodyIds_) {
                    if (id < 0) continue;
                    const int slot = poseHistory->slotOfStructure(id);
                    if (slot < 0) continue;
                    if (!poseHistory->readInterpolated(rt, slot, pe)) continue;
                    vkCtx.setBodyPoseDirect(id, pe.px, pe.py, pe.pz,
                                            pe.qx, pe.qy, pe.qz, pe.qw);
                }
            }

            // 玩家模型位姿：直读共享槽（原子）。位置语义 = 脚底中心，与模型原点一致。
            // playerEntityId 由装配层按类型名解析；-1 表示没有玩家实体，跳过。
            if (playerEntityId > 0 && playerBodyState.valid.load(std::memory_order_relaxed)) {
                // 插值读：MC 同款 —— 由本地单调时钟算出浮点 tick 号，不需要"推进时钟"。
                // 模型与相机取同一个插值结果，否则两者错开一拍会看到相对的抖动。
                double px = 0.0, py = 0.0, pz = 0.0;
                playerBodyState.readInterpolated(px, py, pz);

                // 朝向换算：相机 yaw = 0 朝 -Z，模型 yawDeg = 0 脸朝 +Z，两套约定差一个镜像，
                // 所以取 180° - yaw。yaw 是累加量，不归一化（glm::rotate 接受任意角度）。
                const double yawRad = playerBodyState.yaw.load(std::memory_order_relaxed);
                const double modelYawDeg = 180.0 - yawRad * (180.0 / 3.14159265358979323846);
                vkCtx.setEntityPose(playerEntityId, px, py, pz,
                                    static_cast<float>(modelYawDeg));
            }
            // 第一人称隐藏自身模型：相机就在眼位，模型会糊满整个视野。
            // viewMode：0=自由相机 1=第一人称 2=第三人称（见 CameraState）。
            if (playerEntityId > 0) {
                vkCtx.setEntityVisible(playerEntityId,
                    cameraState.viewMode.load(std::memory_order_relaxed) != 1);
            }

            vkCtx.drawFrame();
            // 本帧收尾：通知主线程可以开始下一次 poll。
            frameGate.markFrameDone();
            // 这里原本有一行 sleep_for(5ms) 做限速，已删除：节奏由 FIFO(vsync) 的
            // vkAcquireNextImageKHR 阻塞决定，帧率上限 = 显示器刷新率；
            // 且 Windows 定时器粒度会把 5ms 请求放大到约 9.5ms（实测 165→62 FPS）。
        }
    });

    // ---- 主线程：GLFW 事件泵，与渲染帧严格一进一出 ----
    // 每次 poll 之前先等上一帧收尾，poll 派发的输入增量正好归属于接下来这一帧；
    // 帧率因此由 FIFO(vsync) 决定，事件泵跟着帧走（不再另设定时器）。
    while (!vkCtx.shouldClose()) {
        if (!frameGate.waitFrameConsumed()) break;
        vkCtx.pollEvents();
        if (vkCtx.shouldClose()) break;   // 本次 poll 里收到关闭请求：不再往下派
        frameGate.markPolled();
    }
    // 关闩：唤醒可能仍在等 poll 的渲染线程，让它退出循环。
    frameGate.shutdown();

    // 优雅停止：模拟两条轨先停（业务 → 生成 → 物理），渲染轨最后停（只读，不阻塞模拟停止）。
    businessLoop.stop();
    if (streamer) streamer->stop();   // 业务线程已停（不再 receiveResults），再停生成线程
    physicsLoop.stop();
    renderLoop.stop();

    vkCtx.shutdown();
    //模组逆序卸载：放在渲染/业务全部停掉之后——
    //注册表里还存着 DLL 提供的字符串指针，提前卸载会变成悬垂引用。
    //卸载前还要先释放"由 DLL 创建、由引擎析构"的对象：
    //height_ 的 destroy 是 DLL 内的函数指针，而 columnGen 是 run() 的局部变量，
    //要到 return 才析构 —— 晚于 unloadAll 就是调用已卸载映像里的地址（UB）。
    streamer.reset();           // 先断开 ChunkStreamer 对生成器的引用
    columnGen.releaseNoise();   // 再让 DLL 亲手销毁自己 new 出来的实例
    engine::modhost::ModLoader::unloadAll();
    std::cout << "引擎退出。\n";
    return 0;
}

// 解析启动参数 --log-level=debug|info|warn|error（缺省 Info）。
// 只认启动参数，不读配置文件——等级是"本次运行想看多细"，不是世界内容。
static engine::core::LogLevel parseLogLevelArg(int argc, char** argv) {
    engine::core::LogLevel level = engine::core::LogLevel::Info;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        const std::string prefix = "--log-level=";
        if (arg.rfind(prefix, 0) == 0) {
            level = engine::core::logParseLevel(arg.substr(prefix.size()), level);
        } else if (arg == "--log-level" && (i + 1) < argc) {
            ++i;
            level = engine::core::logParseLevel(argv[i], level);
        }
    }
    return level;
}

// 解析启动参数 --preset <name>（缺省 default）：决定跑哪个世界预设。
static std::string parsePresetArg(int argc, char** argv) {
    std::string name = "default";
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--preset" && (i + 1) < argc) {
            ++i;
            name = argv[i];
        }
    }
    return name;
}

int main(int argc, char** argv) {
    initConsole();
    chdirToExeDir();

    // 日志：控制台 + logs/engine.log（10MB×5 滚动），异步写不阻塞线程。
    engine::core::logInit(parseLogLevelArg(argc, argv));
    engine::core::logSetThreadName("main");   // 主线程标识；子线程由 ThreadLoop::start 自动登记

    // 临时自检入口：--check-model [模型路径] 只加载实体模型并打印骨骼表，用于核对解析结果。
    // 不给路径时用玩家模组里的模型（模型版权归模组，引擎 assets 里不再留副本）。
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--check-model") continue;
        const std::string modelPath = (i + 1 < argc)
            ? std::string(argv[i + 1])
            : std::string("mods/player_entity/assets/models/entity/PlayerModel.bbmodel");
        engine::assets::EntityModel em;
        const bool ok = engine::assets::loadBbModel(modelPath, em);
        if (!ok) {
            engine::core::logShutdown();
            return 2;
        }
        std::cout << "[实体] 模型=" << modelPath << " 骨骼=" << em.bones.size()
                  << " 贴图=" << em.texturePaths.size()
                  << " 分辨率=" << em.texWidth << "x" << em.texHeight << "\n";
        for (const auto& b : em.bones) {
            std::cout << "  " << b.name
                      << " parent=" << b.parent
                      << " pivot=(" << b.pivot.x << "," << b.pivot.y << "," << b.pivot.z << ")"
                      << " from=(" << b.cubes[0].from.x << "," << b.cubes[0].from.y << "," << b.cubes[0].from.z << ")"
                      << " to=(" << b.cubes[0].to.x << "," << b.cubes[0].to.y << "," << b.cubes[0].to.z << ")\n";
        }
        engine::core::logShutdown();
        return 0;
    }

    const int code = run(parsePresetArg(argc, argv));

    engine::core::logShutdown();   // 停止后台线程并 flush 剩余队列
    return code;
}

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
#include "render/VulkanMesh.hpp"
#include "shared/state/CameraState.hpp"
#include "shared/protocol/Protocol.hpp"
#include "shared/state/PlayerInput.hpp"
#include "registry/BlockRegistry.hpp"
#include "modhost/loader/ModLoader.hpp"
#include "resource/ResourceIndex.hpp"
#include "render/assets/ModelDef.hpp"
#include "render/assets/ModelLoader.hpp"
#include "worldgen/noise/BuiltinNoiseTypes.hpp"
#include "worldgen/noise/NoiseDefTable.hpp"
#include "worldgen/noise/NoiseRegistry.hpp"
#include "worldgen/preset/PresetLoader.hpp"
#include "worldgen/preset/WorldPreset.hpp"
#include "worldgen/generator/ColumnGenerator.hpp"
#include "worldgen/streaming/ChunkStreamer.hpp"

#include <algorithm>
#include <chrono>
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

// 调色板自检：覆盖 4→8→16 两档扩宽，确认扩宽后逐格内容不变、计数正确。
static bool selfTestPalette() {
    // ① 新 chunk 全空
    shared::ChunkData blank;
    if (!blank.isAllAir() || blank.nonAirCount() != 0) return false;
    if (blank.bits() != 4 || blank.paletteSize() != 1) return false;

    // ② 4→8 扩宽（第 17 个条目触发）
    shared::ChunkData a;
    for (int i = 0; i < 20; ++i)
        a.set(i % 16, (i / 16) % 16, 0, static_cast<shared::PaletteEntry>(2000 + i));
    if (a.bits() != 8 || a.paletteSize() != 21) return false;
    for (int i = 0; i < 20; ++i)
        if (a.get(i % 16, (i / 16) % 16, 0) != static_cast<shared::PaletteEntry>(2000 + i))
            return false;
    if (a.nonAirCount() != 20) return false;

    // ③ 8→16 扩宽（第 257 个条目触发），写满 300 项后再逐格核对
    shared::ChunkData b;
    for (int i = 0; i < 300; ++i)
        b.set(i % 16, (i / 16) % 16, i / 256, static_cast<shared::PaletteEntry>(3000 + i));
    if (b.bits() != 16 || b.paletteSize() != 301) return false;
    for (int i = 0; i < 300; ++i)
        if (b.get(i % 16, (i / 16) % 16, i / 256) !=
            static_cast<shared::PaletteEntry>(3000 + i))
            return false;
    for (std::int32_t i = 300; i < shared::CHUNK_VOLUME; ++i)
        if (b.getAt(i) != shared::kAirEntry) return false;
    if (b.nonAirCount() != 300) return false;

    // ④ 写回 Air：字典里仍留着非空气条目，判空必须逐格扫完才返回 true
    shared::ChunkData c;
    const shared::PaletteEntry five = static_cast<shared::PaletteEntry>(5);
    c.set(1, 1, 1, five);
    if (c.get(1, 1, 1) != five) return false;
    c.set(1, 1, 1, shared::kAirEntry);
    if (!c.isAllAir() || c.nonAirCount() != 0) return false;
    return true;
}

// 元素级旋转：绕轴旋转公式 + 原点不动，纯数学自检（不依赖图集与窗口）
static bool selfTestModelRotation() {
    using engine::assets::applyModelRotation;
    using engine::assets::ModelRotation;

    // ① 绕 Z 轴 45°：点 (16,8,8) 绕原点 (8,8,8) → (8+8cos45, 8+8sin45, 8)
    ModelRotation rz;
    rz.origin = glm::vec3{8.0f, 8.0f, 8.0f};
    rz.axis = 'z';
    rz.angle = 45.0f;
    const glm::vec3 a = applyModelRotation(glm::vec3{16.0f, 8.0f, 8.0f}, rz);
    const float e = 8.0f + 8.0f * 0.70710678f;
    if (std::fabs(a.x - e) > 0.01f || std::fabs(a.y - e) > 0.01f || std::fabs(a.z - 8.0f) > 0.01f)
        return false;

    // ② 旋转原点自身不动
    const glm::vec3 o = applyModelRotation(rz.origin, rz);
    if (std::fabs(o.x - 8.0f) > 0.01f || std::fabs(o.y - 8.0f) > 0.01f || std::fabs(o.z - 8.0f) > 0.01f)
        return false;

    // ③ 绕 Y 轴 90°：+X 转到 -Z（右手系）
    ModelRotation ry;
    ry.origin = glm::vec3{8.0f, 8.0f, 8.0f};
    ry.axis = 'y';
    ry.angle = 90.0f;
    const glm::vec3 b = applyModelRotation(glm::vec3{16.0f, 8.0f, 8.0f}, ry);
    if (std::fabs(b.x - 8.0f) > 0.01f || std::fabs(b.y - 8.0f) > 0.01f || std::fabs(b.z - 0.0f) > 0.01f)
        return false;

    // ④ 绕 X 轴 90°：+Y 转到 +Z
    ModelRotation rx;
    rx.origin = glm::vec3{8.0f, 8.0f, 8.0f};
    rx.axis = 'x';
    rx.angle = 90.0f;
    const glm::vec3 d = applyModelRotation(glm::vec3{8.0f, 16.0f, 8.0f}, rx);
    if (std::fabs(d.x - 8.0f) > 0.01f || std::fabs(d.y - 8.0f) > 0.01f || std::fabs(d.z - 16.0f) > 0.01f)
        return false;

    return true;
}

// 元素级旋转的几何落点：直接跑 buildModelVertices，验旋转后顶点的包围盒尺寸。
// 公式自检只证明数学，这里证明"模型里的 rotation 字段确实进到了顶点"。
static bool selfTestModelGeometry() {
    using engine::render::Vertex;

    engine::assets::ModelDef def;
    engine::assets::ModelElement el;
    el.from = glm::vec3{0.0f, 0.0f, 0.0f};
    el.to   = glm::vec3{16.0f, 16.0f, 16.0f};
    for (int f = 0; f < 6; ++f) {
        el.faces[f].texture = "default";
        el.faces[f].cullFace = 0;          // 永不剔 → 6 面全生成
    }
    el.rotation.origin = glm::vec3{8.0f, 8.0f, 8.0f};
    el.rotation.axis = 'z';
    el.rotation.angle = 45.0f;
    def.elements.push_back(el);

    engine::assets::TextureAtlas emptyAtlas;   // 空图集：纹理走兜底格，不影响位置
    const std::vector<Vertex> verts =
        engine::render::mesh::buildModelVertices(glm::vec3{0.0f, 0.0f, 0.0f}, def, 0u, emptyAtlas);
    if (verts.size() != 24) return false;      // 6 面 × 4 顶点

    // 绕 Z 转 45°：X/Y 跨度 16√2 ≈ 22.627，Z 跨度仍 16；÷16 后 → 1.414 / 1.0
    glm::vec3 lo{1e30f, 1e30f, 1e30f};
    glm::vec3 hi{-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : verts) {
        lo = glm::min(lo, v.pos);
        hi = glm::max(hi, v.pos);
    }
    const glm::vec3 span = hi - lo;
    const float diag = 22.6274f * 0.0625f;
    if (std::fabs(span.x - diag) > 0.05f) return false;
    if (std::fabs(span.y - diag) > 0.05f) return false;
    if (std::fabs(span.z - 1.0f) > 0.05f) return false;

    // 同一元素不旋转：三轴跨度都应是 1.0（确认走的是同一条路径的恒等分支）
    def.elements.front().rotation.angle = 0.0f;
    const std::vector<Vertex> plain =
        engine::render::mesh::buildModelVertices(glm::vec3{0.0f, 0.0f, 0.0f}, def, 0u, emptyAtlas);
    if (plain.size() != 24) return false;
    glm::vec3 lo2{1e30f, 1e30f, 1e30f};
    glm::vec3 hi2{-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : plain) {
        lo2 = glm::min(lo2, v.pos);
        hi2 = glm::max(hi2, v.pos);
    }
    const glm::vec3 span2 = hi2 - lo2;
    if (std::fabs(span2.x - 1.0f) > 0.01f) return false;
    if (std::fabs(span2.y - 1.0f) > 0.01f) return false;
    if (std::fabs(span2.z - 1.0f) > 0.01f) return false;

    return true;
}

// 引擎主流程：装配四轨 → 启动 → 主线程事件循环 → 优雅停止。
// presetName：世界预设名（不带 .json），由启动参数 --preset 决定。
static int run(const std::string& presetName) {
    initConsole();
    std::cout << "=== 沙盒引擎 ===\n";

    // 资源索引：先登记引擎自带的 assets/ 与 data/，再由 ModLoader 追加各模组目录下的同名子目录。
    // 登记顺序即冲突优先级：先登记者优先。
    resource::g_resourceIndex.addRoot(resource::ResourceKind::Assets, "", "assets");
    resource::g_resourceIndex.addRoot(resource::ResourceKind::Data, "", "data");

    //方块注册表（#39）：引擎不再写死任何方块，内容全部由 mods/blocks.dll 注册。
    //顺序：loadAll（blocks.dll 按文件名排序最靠前，独占 id 1..6）→ freeze → 按名字解析内置 id。
    //晚于 freeze 的注册会被 Registry::add 静默拒绝；整条链必须早于 vkCtx.init（图集要遍历注册表）。
    const auto modCount = engine::modhost::ModLoader::loadAll("mods");
    engine::registry::freezeBlockRegistry();
    engine::registry::resolveBuiltinBlockIds();
    std::cout << "方块注册表：共 " << engine::registry::g_blockRegistry.size()
              << " 个（成功加载模组 " << modCount << " 个），frozen="
              << std::boolalpha << engine::registry::g_blockRegistry.frozen() << "\n";
    std::cout << "资源索引：assets "
              << resource::g_resourceIndex.count(resource::ResourceKind::Assets)
              << " 个文件，data "
              << resource::g_resourceIndex.count(resource::ResourceKind::Data)
              << " 个文件\n";

    //模型注册表：方块注册之后、Vulkan 初始化之前——
    //图集 build 会读 assets/atlases/blocks.json 的纹理声明，同时核对注册表引用的纹理名
    engine::assets::loadBlockModels();

    //世界生成子系统（#33/#34）：噪声注册 → 行为自检 → 锁表。
    //注册分两批，顺序固定：
    //  ① 模组（上面 loadAll 已调完，各 DLL 在 onLoad 里 registerNoise）
    //  ② 引擎内置 flat（自举项：删掉所有 DLL 后，只剩它仍能跑平地）
    //freeze 之后只读；晚到的注册会被 NoiseRegistry::registerType 直接拒绝。
    engine::worldgen::initBuiltinNoiseTypes();
    const bool noiseOk = engine::worldgen::selfTestNoiseTypes();
    engine::worldgen::NoiseRegistry::freeze();

    //区块调色板（ChunkData）：4/8/16 位扩宽 + 判空自检，装配期跑一次。
    const bool paletteOk = selfTestPalette();

    //元素级旋转（ModelElement::rotation）：绕轴旋转公式自检。
    const bool modelRotOk = selfTestModelRotation();

    //元素级旋转的几何落点：rotation 字段 → buildModelVertices 顶点位置。
    const bool modelGeoOk = selfTestModelGeometry();

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
    std::cout << "区块调色板：4/8/16 位扩宽 + 判空自检"
              << (paletteOk ? "通过" : "失败") << "\n";
    std::cout << "模型元素旋转：绕轴旋转自检"
              << (modelRotOk ? "通过" : "失败") << "\n";
    std::cout << "模型元素旋转：顶点几何自检"
              << (modelGeoOk ? "通过" : "失败") << "\n";

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
    {
        const auto ps = businessWorld.world().paletteStats();
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
                const auto drained = streamer->drainResults(
                    businessWorld.world(),
                    engine::worldgen::ChunkStreamer::kMaxApplyPerTick);
                // 世界 chunk 集合有变化（加载或卸载）→ 物理侧必须重下一次全量方块集。
                // 只认"卸载"不够：卸载与分批加载错在不同 tick，最后一次加载若不重同步，
                // 物理侧就停在旧的中间态快照（实测：世界 90 列、物理仍 81 列）。
                // 稳态时两者都为 0（不再触发），开销只在滑动的那几个 tick。
                if (drained.evictedChunks > 0 || drained.appliedColumns > 0)
                    businessWorld.requestPhysicsResync();
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
            level = engine::core::parseLogLevel(arg.substr(prefix.size()), level);
        } else if (arg == "--log-level" && (i + 1) < argc) {
            ++i;
            level = engine::core::parseLogLevel(argv[i], level);
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
    engine::core::initLog(parseLogLevelArg(argc, argv));
    engine::core::setLogThreadName("main");   // 主线程标识；子线程由 ThreadLoop::start 自动登记

    const int code = run(parsePresetArg(argc, argv));

    engine::core::shutdownLog();   // 停止后台线程并 flush 剩余队列
    return code;
}

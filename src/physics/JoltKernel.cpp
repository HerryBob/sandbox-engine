#include "physics/JoltKernel.hpp"

// Jolt 头顺序有要求：Jolt.h 必须最先（它定义 JPH_ 平台/配置宏）
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>

#include <cmath>
#include <string>

#include "core/Log.hpp"

namespace engine::physics {
namespace {

// v1 的层划分：两层就够（静态地形 / 可动结构）。将来加玩家、触发器再扩。
constexpr JPH::ObjectLayer kObjLayerNonMoving = 0;
constexpr JPH::ObjectLayer kObjLayerMoving    = 1;
constexpr JPH::uint        kNumObjectLayers   = 2;
constexpr JPH::uint        kNumBroadPhaseLayers = 2;

bool g_joltReady = false;

#ifdef JPH_ENABLE_ASSERTS
// Jolt 断言钩子：默认实现会 __debugbreak（无调试器时直接终止进程、什么都不打印）。
// 改成写进日志并返回 false（不触发断点），这样断言能被看见而不是"静默退出"。
bool joltAssertFailed(const char* inExpression, const char* inMessage,
                      const char* inFile, JPH::uint inLine) {
    ENGINE_LOG_ERROR("Physics",
        std::string("Jolt 断言失败: ") + (inExpression ? inExpression : "?") +
        (inMessage ? (std::string(" — ") + inMessage) : std::string()) +
        " @ " + (inFile ? inFile : "?") + ":" + std::to_string(inLine));
    return false;   // 不触发断点，继续跑
}
#endif

} // namespace

bool initJoltKernel() {
    if (g_joltReady) return true;
    JPH::RegisterDefaultAllocator();
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = &joltAssertFailed;
#endif
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    g_joltReady = true;
    return true;
}

void shutdownJoltKernel() {
    if (!g_joltReady) return;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    g_joltReady = false;
}

bool isJoltKernelReady() { return g_joltReady; }

bool selfTestJoltKernel() {
    // ===== 临时阶段标记（定位卡在哪一步，定位完删）=====
    if (!initJoltKernel()) return false;

    // 层表：ObjectLayer → BroadPhaseLayer 的映射 + 哪些层之间会碰撞
    const JPH::BroadPhaseLayer bpNonMoving(0);
    const JPH::BroadPhaseLayer bpMoving(1);

    JPH::BroadPhaseLayerInterfaceTable bpLayers(kNumObjectLayers, kNumBroadPhaseLayers);
    bpLayers.MapObjectToBroadPhaseLayer(kObjLayerNonMoving, bpNonMoving);
    bpLayers.MapObjectToBroadPhaseLayer(kObjLayerMoving, bpMoving);

    JPH::ObjectLayerPairFilterTable objPairFilter(kNumObjectLayers);
    objPairFilter.EnableCollision(kObjLayerNonMoving, kObjLayerMoving);
    objPairFilter.EnableCollision(kObjLayerMoving, kObjLayerMoving);

    JPH::ObjectVsBroadPhaseLayerFilterTable objVsBpFilter(
        bpLayers, kNumBroadPhaseLayers, objPairFilter, kNumObjectLayers);

    // 下面这三个对象必须比 system 活得久（PhysicsSystem 只存引用）
    JPH::PhysicsSystem system;
    system.Init(1024, 0, 1024, 1024, bpLayers, objVsBpFilter, objPairFilter);
    system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

    JPH::BodyInterface& bodies = system.GetBodyInterface();

    // 地面：半高 0.5，中心 y = -0.5 → 顶面正好 y = 0
    // shape 必须堆分配并用 Ref 托管：BodyCreationSettings / Body 会对它加引用计数，
    // 栈对象会在计数归零前析构 → 触发 Jolt 的 Reference.h:40 断言。
    JPH::RefConst<JPH::Shape> groundShape = new JPH::BoxShape(JPH::Vec3(20.0f, 0.5f, 20.0f));
    JPH::BodyCreationSettings groundSettings(
        groundShape.GetPtr(), JPH::RVec3(0.0, -0.5, 0.0), JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, kObjLayerNonMoving);
    const JPH::BodyID groundId = bodies.CreateAndAddBody(groundSettings, JPH::EActivation::DontActivate);

    // 箱子：半边长 0.5，初始中心 y = 5（自由落体 4.5 格）
    JPH::RefConst<JPH::Shape> boxShape = new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));
    JPH::BodyCreationSettings boxSettings(
        boxShape.GetPtr(), JPH::RVec3(0.0, 5.0, 0.0), JPH::Quat::sIdentity(),
        JPH::EMotionType::Dynamic, kObjLayerMoving);
    const JPH::BodyID boxId = bodies.CreateAndAddBody(boxSettings, JPH::EActivation::Activate);
    if (boxId.IsInvalid()) {
        ENGINE_LOG_ERROR("Physics", "Jolt 自检：箱子刚体创建失败");
        return false;
    }

    // 引擎的定时策略：25Hz 固定步长 + 每步 2 次碰撞子步（与文档 §六 一致）
    JPH::TempAllocatorImpl tempAllocator(10 * 1024 * 1024);
    JPH::JobSystemSingleThreaded jobSystem(2048);
    constexpr float kFixedDt = 1.0f / 25.0f;
    constexpr int   kSteps   = 100;             // 4 秒，足够落地并静止
    for (int i = 0; i < kSteps; ++i)
        system.Update(kFixedDt, 2, &tempAllocator, &jobSystem);

    const JPH::RVec3 pos = bodies.GetPosition(boxId);
    const float x = pos.GetX(), y = pos.GetY(), z = pos.GetZ();

    // 期望：中心停在"地面顶面 0 + 半高 0.5" = 0.5；水平方向不该漂移
    const bool landed   = std::fabs(y - 0.5f) <= 0.05f;
    const bool centered = std::fabs(x) <= 0.05f && std::fabs(z) <= 0.05f;
    if (!landed || !centered) {
        ENGINE_LOG_ERROR("Physics",
            "Jolt 自检失败：箱子落点 x=" + std::to_string(x) +
            " y=" + std::to_string(y) + " z=" + std::to_string(z) +
            "（期望 y≈0.5，x/z≈0）");
        return false;
    }

    // Jolt 要求 PhysicsSystem 析构前所有刚体都已移除（否则析构里的断言会 __debugbreak）
    bodies.RemoveBody(boxId);
    bodies.RemoveBody(groundId);
    return true;
}

} // namespace engine::physics

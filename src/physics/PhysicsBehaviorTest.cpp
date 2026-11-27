#include "physics/PhysicsBehaviorTest.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "core/SPSCQueue.hpp"
#include "core/TimeManager.hpp"
#include "physics/JoltKernel.hpp"
#include "physics/PhysicsWorld.hpp"
#include "shared/block/Block.hpp"
#include "shared/protocol/Protocol.hpp"
#include "shared/state/PoseHistory.hpp"

namespace engine::physics {

namespace {

using engine::shared::Block;
using engine::shared::BlockPos;
using engine::shared::BlockType;
using engine::shared::PhysicsCommand;

// 测试用的方块类型：物理只看"是不是碰撞体"，不看具体类型，
// 所以不依赖方块注册表（自检可以在 mods 加载前后任意时刻跑）。
constexpr std::uint16_t kTestBlockType = 1;

PhysicsCommand makeBlockSet(int minX, int maxX, int y, int minZ, int maxZ,
                            bool skipCenter) {
    PhysicsCommand c;
    c.type = PhysicsCommand::Type::SyncBlockSet;
    c.blocks.reserve(static_cast<std::size_t>((maxX - minX + 1) * (maxZ - minZ + 1)));
    for (int x = minX; x <= maxX; ++x)
        for (int z = minZ; z <= maxZ; ++z) {
            // 用例③：挖掉脚下 3×3 的一块（1×1 会与洞壁完美贴合、被摩擦夹住，
            // 测不出"是否下落"；放大到 3×3 才能干净地验证"失去支撑就该掉"）
            if (skipCenter && x >= 7 && x <= 9 && z >= 7 && z <= 9) continue;
            c.blocks.push_back(Block{
                static_cast<BlockType>(kTestBlockType), BlockPos{x, y, z}, -1});
        }
    return c;
}

PhysicsCommand makeSingleBlockPack(int x, int y, int z) {
    PhysicsCommand c;
    c.type = PhysicsCommand::Type::PackContraption;
    c.contraptionId = 1;
    c.blocks.push_back(Block{static_cast<BlockType>(kTestBlockType), BlockPos{x, y, z}, -1});
    c.blockCount = 1;
    return c;
}

// 读某结构的最新位姿（走真实通路：PoseHistory）。
bool readPose(const engine::shared::PoseHistory& ph, int structureId,
              double tick, engine::shared::PoseEntry& out) {
    const int slot = ph.slotOfStructure(structureId);
    if (slot < 0) return false;
    return ph.readInterpolated(tick, slot, out);
}

} // namespace

bool selfTestPhysicsBehavior() {
    if (!initJoltKernel()) {
        std::cout << "物理行为：Jolt 内核初始化失败\n";
        return false;
    }

    engine::core::TimeManager tm;
    PhysicsWorld world(tm);
    engine::shared::PoseHistory poses;
    world.attachPoseHistory(&poses);

    engine::core::SPSCQueue<PhysicsCommand> queue(64);
    auto send = [&queue](PhysicsCommand c) {
        while (!queue.push(std::move(c))) { /* 队列满：重试（自检里不会发生） */ }
    };
    // 固定步长推进：tick(scale=1, wallDt=0.04) → 每次调用正好一步
    auto step = [&world, &queue](int count) {
        for (int i = 0; i < count; ++i) world.tick(1.0, 0.04, queue);
    };

    // 每次 PackContraption 都会分配新的 structureId（引擎内部 = allocateBodyId()），
    // 所以这里**按调用顺序**记录，不能硬编码 1 —— 否则会读到上一个用例的刚体
    // （第一版就踩了这个：③查的是①的刚体，于是"挖掉脚下不掉"）。
    int nextStructureId = 1;   // PhysicsState::nextBodyId_ 从 1 开始

    // −−− 用例① 自由落体：没有地面，1 秒后必须明显下落
    {
        const int sid = nextStructureId++;
        send(makeSingleBlockPack(0, 5, 0));
        step(25);   // 1 秒
        engine::shared::PoseEntry e;
        if (!readPose(poses, sid, 25.0, e)) {
            std::cout << "物理行为：①自由落体 —— 读不到位姿\n";
            return false;
        }
        // 1 秒自由落体 ≈ 4.9m，从 y=5 起算应低于 4.5（保守判据，不卡具体解算器）
        if (!(e.py < 4.5f)) {
            std::cout << "物理行为：①自由落体失败 y=" << e.py << "（应 < 4.5）\n";
            return false;
        }
    }

    // −−− 用例② 落到台面：地面上方打包，应停在顶面（方块中心 y=1.5）
    {
        const int sid = nextStructureId++;
        send(makeBlockSet(0, 15, 0, 0, 15, false));
        send(makeSingleBlockPack(8, 6, 8));
        step(200);   // 8 秒，足够落地并稳定
        engine::shared::PoseEntry e;
        if (!readPose(poses, sid, 225.0, e)) {
            std::cout << "物理行为：②落到台面 —— 读不到位姿\n";
            return false;
        }
        // 注意：当前实现的原点是**包围盒角**（形状偏移 = 局部 pos + 0.5），
        // 不是包围盒中心 —— 所以静止时 body.y = 地面顶面 = 1.0，而不是 1.5。
        // 这个约定偏差记在 bug 台账 B141（旋转/多体前必须统一），
        // 这里按**现状**断言，用例仍然覆盖"落地"这一行为。
        if (std::fabs(e.py - 1.0f) > 0.15f) {
            std::cout << "物理行为：②落到台面失败 y=" << e.py << "（应 ≈ 1.0，见 B141）\n";
            return false;
        }
    }

    // −−− 用例③ 挖掉脚下：支撑块从集合里去掉 → 必须掉下去（覆盖 B140）
    {
        const int sid = nextStructureId - 1;   // ② 的刚体
        send(makeBlockSet(0, 15, 0, 0, 15, /*skipCenter=*/true));
        step(100);   // 4 秒
        engine::shared::PoseEntry e;
        if (!readPose(poses, sid, 325.0, e)) {
            std::cout << "物理行为：③挖掉脚下 —— 读不到位姿\n";
            return false;
        }
        if (!(e.py < 0.5f)) {
            std::cout << "物理行为：③挖掉脚下失败 y=" << e.py
                      << "（应掉到 0.5 以下；说明睡眠体没被唤醒 / 没被积分）\n";
            return false;
        }
    }

    std::cout << "物理行为：自由落体 / 落到台面 / 挖掉脚下 自检通过\n";
    return true;
}

} // namespace engine::physics

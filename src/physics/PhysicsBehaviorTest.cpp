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
        // 位姿语义（B145 起）：对外发布的刚体位姿 = **结构格点原点**
        // （= 质心位姿 − R·bias）。单块落在地面（顶面 y=1）时：COM=1.5、bias=0.5
        // → 格点原点 y = 1.0。渲染端顶点就用 p + 0.5，两边都与 bias 无关。
        if (std::fabs(e.py - 1.0f) > 0.15f) {
            std::cout << "物理行为：②落到台面失败 y=" << e.py << "（应 ≈ 1.0）\n";
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

    // −−− 用例④ 约定自检：位姿 = 方块中心的平均（偶数尺寸时 bias = 1.0）
    // 2 个方块 z=20,21：origin_z=20、bias_z=1.0 ⇒ 几何中心 z = 20+1.0 = 21.0，
    // 两块中心 z 分别是 20.5 / 21.5，平均正好 21.0。
    // 旧约定（角原点）会给出 20.0 —— 这条用例就是 B141 的回归防线。
    {
        const int sid = nextStructureId++;
        engine::shared::PhysicsCommand pack;
        pack.type = engine::shared::PhysicsCommand::Type::PackContraption;
        pack.contraptionId = 1;
        pack.blocks.push_back(engine::shared::Block{
            static_cast<engine::shared::BlockType>(kTestBlockType),
            engine::shared::BlockPos{20, 6, 20}, -1});
        pack.blocks.push_back(engine::shared::Block{
            static_cast<engine::shared::BlockType>(kTestBlockType),
            engine::shared::BlockPos{20, 6, 21}, -1});
        pack.blockCount = 2;
        send(std::move(pack));
        step(1);   // 只走一步：验证"打包瞬间"的位姿约定（重力只让它动 ~1.5cm）
        engine::shared::PoseEntry e;
        if (!readPose(poses, sid, 426.0, e)) {
            std::cout << "物理行为：④中心约定 —— 读不到位姿\n";
            return false;
        }
        // 期望 (20.5, 6.5, 21.0)：x/y 各 +0.5（奇数尺寸 1 块宽），z +1.0（偶数尺寸 2 块）
        // 格点原点 = 整数基准（x/y 各 20/6，z 因两块而取 20）
        const bool ok = std::fabs(e.px - 20.0f) < 0.05f &&
                        std::fabs(e.py - 6.0f) < 0.05f &&
                        std::fabs(e.pz - 20.0f) < 0.05f;
        if (!ok) {
            std::cout << "物理行为：④中心约定失败 pos=(" << e.px << "," << e.py << ","
                      << e.pz << ")（应 ≈ (20,6,20)）\n";
            return false;
        }
    }

    // −−− 用例⑤ 刚体上加/拆方块（InteractStructure）
    // 从正下方向上打射线：命中刚体 → 在命中面外（上方）加一块；再从上向下打 → 拆掉。
    // 判决依据是物理回执里的**新局部方块表**（渲染端就是拿它重建网格的）。
    {
        engine::core::SPSCQueue<engine::shared::PhysicsResult> results(64);
        auto take = [&world, &results](engine::shared::PhysicsResult& out) {
            world.pushResultsToBusiness(results);
            while (results.pop(out)) {
                if (out.type == engine::shared::PhysicsResult::Type::StructureEdited ||
                    out.type == engine::shared::PhysicsResult::Type::StructureEditMiss) return true;
            }
            return false;
        };

        const int sid = nextStructureId++;
        send(makeSingleBlockPack(30, 6, 30));   // 悬空一块（下面没有地面，先让它掉一会儿）
        step(2);

        // ① 从下往上打：命中底面 → 在上方加一块（raction = 1）
        {
            engine::shared::PhysicsCommand in;
            in.type = engine::shared::PhysicsCommand::Type::InteractStructure;
            in.rox = 30.5; in.roy = 0.0; in.roz = 30.5;
            in.rdx = 0.0; in.rdy = 1.0; in.rdz = 0.0;
            in.rmaxDist = 40.0; in.raction = 1;
            in.blockType = static_cast<int>(kTestBlockType);
            send(std::move(in));
            step(2);

            engine::shared::PhysicsResult r;
            if (!take(r) || r.type != engine::shared::PhysicsResult::Type::StructureEdited) {
                std::cout << "物理行为：⑤刚体加方块 —— 没有收到 StructureEdited\n";
                return false;
            }
            if (r.contraptionId != sid || r.blocks.size() != 2) {
                std::cout << "物理行为：⑤刚体加方块失败 id=" << r.contraptionId
                          << " 块数=" << r.blocks.size() << "（应 2 块）\n";
                return false;
            }
        }

        // ② 从上往下打：命中顶面 → 拆掉顶块（raction = 0）
        {
            // 用刚体**当前真实位姿**构造射线（它会受重力下落，硬编码高度不可靠）
            engine::shared::PoseEntry before;
            double topY = 20.0;
            if (readPose(poses, sid, 429.0, before)) topY = static_cast<double>(before.py) + 5.0;            engine::shared::PhysicsCommand in;
            in.type = engine::shared::PhysicsCommand::Type::InteractStructure;
            in.rox = 30.5; in.roy = topY; in.roz = 30.5;
            in.rdx = 0.0; in.rdy = -1.0; in.rdz = 0.0;
            in.rmaxDist = 30.0; in.raction = 0;
            send(std::move(in));
            step(2);

            engine::shared::PhysicsResult r;
            const bool got = take(r);
            if (!got || r.type != engine::shared::PhysicsResult::Type::StructureEdited) {
                std::cout << "物理行为：⑤刚体拆方块 —— 回执 got=" << got
                          << " type=" << static_cast<int>(r.type)
                          << " 块数=" << r.blocks.size() << "\n";
                return false;
            }
            if (r.blocks.size() != 1) {
                std::cout << "物理行为：⑤刚体拆方块失败 块数=" << r.blocks.size() << "（应 1 块）\n";
                return false;
            }
        }

        // ③ 打空（射线朝天空）→ 必须是 StructureEditMiss
        {
            engine::shared::PhysicsCommand in;
            in.type = engine::shared::PhysicsCommand::Type::InteractStructure;
            in.rox = 30.5; in.roy = 40.0; in.roz = 30.5;
            in.rdx = 0.0; in.rdy = 1.0; in.rdz = 0.0;
            in.rmaxDist = 20.0; in.raction = 0;
            send(std::move(in));
            step(2);
            engine::shared::PhysicsResult r;
            if (!take(r) || r.type != engine::shared::PhysicsResult::Type::StructureEditMiss) {
                std::cout << "物理行为：⑤打空应当返回 StructureEditMiss\n";
                return false;
            }
        }
    }

    std::cout << "物理行为：自由落体 / 落到台面 / 挖掉脚下 / 中心约定 / 刚体加拆方块 自检通过\n";
    return true;
}

} // namespace engine::physics

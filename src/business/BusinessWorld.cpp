#include "business/BusinessWorld.hpp"
#include "core/Log.hpp"

#include <cmath>
#include <sstream>

namespace engine::business {

BusinessWorld::BusinessWorld(engine::core::TimeManager& tm): tm_(tm), lastLog_(std::chrono::steady_clock::now()) {
    // 原型：初始世界放一艘 3×3×3 木头飞船 + 一块飞行路径上的石头障碍物。
    // blocks_ 构造时写入初始内容；此后仅业务线程读写（放置/破坏/打包取数），
    // main 只在三轨停止后读取 blockCount/findBlock，无并发写。
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            for (int z = -1; z <= 1; ++z) {
                blocks_.push_back(engine::shared::Block{
                    engine::shared::BlockType::Wood,
                    engine::shared::BlockPos{x, y, z}});
            }
        }
    }
    // 障碍物放在飞船起飞路径 (50,0,0)：飞船撞上 → 碰撞事件 → 业务裁决破坏它。
    // 打包指令只打包飞船区域方块，障碍物留在静态世界（打包 = 选择子集，架构 2.7）。
    blocks_.push_back(engine::shared::Block{
        engine::shared::BlockType::Stone,
        engine::shared::BlockPos{50, 0, 0}});
}

void BusinessWorld::advanceTick(double scale) {
    // 20TPS：每个业务 tick 推进 dt=0.05s，受 time_scale 缩放。
    // simTime_ 是跨线程共享（main 线程读取），必须走原子读改写。
    // 演示事件（放置/打包/改造）由 demo::DemoDriver 在业务 tick 边界驱动，
    // 本类只负责世界状态推进与裁决。
    simTime_.store(simTime_.load(std::memory_order_relaxed) + 0.05 * scale,
                   std::memory_order_relaxed);
    ticks_.fetch_add(1, std::memory_order_relaxed);
    maybeLog();
}

bool BusinessWorld::syncAll() {
    // 请求全量同步：排队一条 SyncBlockSet（初始世界/世界重建时用）。
    // 返回 false 表示已有待发指令（原型：一次只排一条，防止覆盖）。
    if (wantSync_ || wantPack_ || wantUnpack_ || wantPlace_ || wantRemove_) return false;
    wantSync_ = true;
    return true;
}
bool BusinessWorld::packContraption() {
    // 请求打包：此刻从 blocks_ 快照"飞船区域"方块（|x|,|y|,|z|<=1，业务裁决
    // 选择哪些方块进刚体），排队一条 PackContraption 指令。
    // 障碍物等主世界静态方块留在静态碰撞体集合（架构 2.7 物理形态转移）。
    if (wantSync_ || wantPack_ || wantUnpack_) return false;
    packBlocks_.clear();
    for (const auto& b : blocks_) {
        if (std::abs(b.pos.x) <= 1 && std::abs(b.pos.y) <= 1 && std::abs(b.pos.z) <= 1)
            packBlocks_.push_back(b);
    }
    // 记录刚体内方块坐标：碰撞裁决保护（刚体自身方块不可被撞毁）。
    packedBlocks_.clear();
    for (const auto& b : packBlocks_) packedBlocks_.insert(b.pos);
    wantPack_ = true;
    return true;
}
bool BusinessWorld::unpackContraption() {
    // 请求解包：排队一条 UnpackContraption（目标刚体 ID 来自打包回执）。
    if (wantSync_ || wantPack_ || wantUnpack_) return false;
    wantUnpack_ = true;
    return true;
}
int BusinessWorld::packedId() const {
    return packedId_;
}

void BusinessWorld::syncToPhysics(engine::core::SPSCQueue<engine::shared::PhysicsCommand>& q) {
    // 业务 Tick 边界：下发指令。一次只发一条（原型节奏），
    // 优先级：全量同步 → 打包 → 解包 → 放置 → 破坏 → 常规移动。
    engine::shared::PhysicsCommand cmd;
    if (wantSync_) {
        // 全量同步：把业务世界所有方块（含类型/坐标）下发，物理重建静态碰撞体集合。
        cmd.type = engine::shared::PhysicsCommand::Type::SyncBlockSet;
        cmd.blocks = blocks_;
        if (q.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantSync_ = false;
        }
    } else if (wantPack_) {
        // 打包指令：方块列表在 packContraption 时已从 blocks_ 快照（飞船区域），
        // 物理把对应静态碰撞体转移进刚体（架构 2.7 物理形态转移）。
        cmd.type = engine::shared::PhysicsCommand::Type::PackContraption;
        cmd.ivx = 1.0; // 业务裁决：飞船以 1 格/秒 沿 x 方向飞行
        cmd.blocks = packBlocks_;
        cmd.blockCount = static_cast<int>(packBlocks_.size());
        if (q.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantPack_ = false;
        }
    } else if (wantUnpack_) {
        // 解包指令：把刚体拆回方块（目标刚体 ID 来自打包回执）。
        cmd.type = engine::shared::PhysicsCommand::Type::UnpackContraption;
        cmd.contraptionId = packedId_;
        if (q.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantUnpack_ = false;
        }
    } else if (wantPlace_) {
        // 放置指令：业务裁决已通过（位置合法），物理按目标域建碰撞体/加入刚体。
        cmd.type = engine::shared::PhysicsCommand::Type::PlaceBlock;
        cmd.x = placePos_.x; cmd.y = placePos_.y; cmd.z = placePos_.z;
        cmd.blockType = placeType_;
        cmd.contraptionId = placeCid_; // -1=主世界，≥0=刚体（飞船改造）
        if (q.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantPlace_ = false;
        }
    } else if (wantRemove_) {
        // 破坏指令：业务裁决已通过（方块存在），物理按目标域删碰撞体/刚体方块。
        cmd.type = engine::shared::PhysicsCommand::Type::RemoveBlock;
        cmd.x = removePos_.x; cmd.y = removePos_.y; cmd.z = removePos_.z;
        cmd.contraptionId = removeCid_; // -1=主世界，≥0=刚体（飞船改造）
        if (q.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantRemove_ = false;
        }
    } else {
        // 常规演示指令：移动方块。
        cmd.type = engine::shared::PhysicsCommand::Type::MoveBlock;
        cmd.dx   = 1;
        if (q.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
        }
        // push 失败（队列满）则丢弃：慢放时物理消化变慢、命令堆积，可观察水位现象。
    }
}

void BusinessWorld::receiveFromPhysics(
    engine::core::SPSCQueue<engine::shared::PhysicsResult>& q) {
    // 业务 Tick 边界：收物理结果（位置快照 + 打包回执）。
    engine::shared::PhysicsResult res;
    while (q.pop(res)) {
        resultsReceived_.fetch_add(1, std::memory_order_relaxed);
        switch (res.type) {
        case engine::shared::PhysicsResult::Type::BlockMoved:
            blockX_.store(res.x, std::memory_order_relaxed);
            blockY_.store(res.y, std::memory_order_relaxed);
            blockZ_.store(res.z, std::memory_order_relaxed);
            break;
        case engine::shared::PhysicsResult::Type::ContraptionPacked:
            packedId_ = res.contraptionId;
            contraptionState_.store(1, std::memory_order_relaxed);
            break;
        case engine::shared::PhysicsResult::Type::ContraptionUnpacked:
            contraptionState_.store(2, std::memory_order_relaxed);
            packedBlocks_.clear(); // 刚体已拆回方块，碰撞保护集合失效
            // 解包闭环（架构 2.7）：用物理回执的世界坐标方块重建 blocks_。
            // 业务权威的坐标与物理回写位置对齐——飞船飞了多远，方块表就记多远，
            // 不再停留在打包前的局部坐标 (-1..1)。
            {
                std::lock_guard<std::mutex> lock(blocksMutex_);
                blocks_ = res.blocks;
            }
            break;
        case engine::shared::PhysicsResult::Type::RigidBodyPos:
            // 刚体（飞船）位置快照：业务视角更新飞船坐标。
            shipX_.store(res.rx, std::memory_order_relaxed);
            shipY_.store(res.ry, std::memory_order_relaxed);
            shipZ_.store(res.rz, std::memory_order_relaxed);
            break;
        case engine::shared::PhysicsResult::Type::BlockSetSize:
            // 物理静态碰撞体数量快照（合并语义：只留最新值）。
            physicsBlockCount_.store(res.blockSetSize, std::memory_order_relaxed);
            break;
        case engine::shared::PhysicsResult::Type::CollisionEvent: {
            // 碰撞事件（架构 7.3 撞击破坏）：业务裁决直接破坏被撞的静态方块。
            // 与玩家 removeBlock 不同：重复碰撞事件不算"玩家请求被拒"，不计数；
            // 也计入 collisions_ 供演示/断言观测。
            collisions_.fetch_add(1, std::memory_order_relaxed);
            // 刚体内方块不可被撞毁（飞船自身结构不因撞击塌陷，原型）。
            if (contraptionState_.load(std::memory_order_relaxed) == 1 &&
                packedBlocks_.count(engine::shared::BlockPos{res.cx, res.cy, res.cz}))
                break;
            const engine::shared::BlockPos cp{res.cx, res.cy, res.cz};
            {
                std::lock_guard<std::mutex> lock(blocksMutex_);
                for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
                    // 只破坏主世界静态方块（刚体自身方块受 packedBlocks_ 保护，已在上面拦截）。
                    if (it->pos == cp && it->contraptionId == -1) {
                        blocks_.erase(it);
                        wantRemove_ = true;
                        removePos_ = cp; // 下发 RemoveBlock：物理删碰撞体
                        removeCid_ = -1;
                        break;
                    }
                }
            }
            break;
        }
        }
    }
}

double BusinessWorld::simTime() const {
    return simTime_.load(std::memory_order_relaxed);
}
long long BusinessWorld::tickCount() const {
    return ticks_.load(std::memory_order_relaxed);
}
long long BusinessWorld::commandsSent() const {
    return commandsSent_.load(std::memory_order_relaxed);
}
long long BusinessWorld::resultsReceived() const {
    return resultsReceived_.load(std::memory_order_relaxed);
}
int BusinessWorld::blockX() const { return blockX_.load(std::memory_order_relaxed); }
int BusinessWorld::blockY() const { return blockY_.load(std::memory_order_relaxed); }
int BusinessWorld::blockZ() const { return blockZ_.load(std::memory_order_relaxed); }

const std::vector<engine::shared::Block>& BusinessWorld::blocks() const {
    return blocks_;
}
std::vector<engine::shared::Block> BusinessWorld::renderSnapshot() const {
    // 第 4 轨只读：与业务线程结构性修改并发，锁内拷贝一份安全快照。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    return blocks_;
}
std::size_t BusinessWorld::blockCount() const {
    // main 线程伪渲染/断言与业务线程结构性修改可能并发——加锁读。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    return blocks_.size();
}
const engine::shared::Block* BusinessWorld::findBlock(
    const engine::shared::BlockPos& p, int contraptionId) const {
    // 调用方需保证无并发写（业务线程持锁调用 / main 在三轨停止后调用）。
    for (const auto& b : blocks_) {
        if (b.pos == p && b.contraptionId == contraptionId) return &b;
    }
    return nullptr;
}
bool BusinessWorld::placeBlock(const engine::shared::BlockPos& p,
                               engine::shared::BlockType type, int contraptionId) {
    // 裁决①：目标域是刚体时飞船必须处于打包状态（刚体存在性）。
    // 飞船改造语义：打包期间主世界与刚体空间的方块增删都允许（不再结构冻结），
    // 刚体域变更由物理侧增量重算碰撞箱（Sable 思想）。
    if (contraptionId >= 0 &&
        contraptionState_.load(std::memory_order_relaxed) != 1) {
        placementsRejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::lock_guard<std::mutex> lock(blocksMutex_);
    // 裁决②：同域位置已被占用（主世界与刚体空间相互独立，坐标可重叠）。
    if (findBlock(p, contraptionId) != nullptr) {
        placementsRejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    blocks_.push_back(engine::shared::Block{type, p, contraptionId});
    blocksPlaced_.fetch_add(1, std::memory_order_relaxed);
    // 自动排队 PlaceBlock 指令（下次 syncToPhysics 发出）。
    // 原型注意：若上一条指令因队列满未发出，本请求会覆盖待发坐标（低概率，接受）。
    wantPlace_ = true;
    placePos_ = p;
    placeType_ = static_cast<int>(type);
    placeCid_ = contraptionId;
    return true;
}

bool BusinessWorld::removeBlock(const engine::shared::BlockPos& p, int contraptionId) {
    // 裁决①：目标域是刚体时飞船必须处于打包状态（刚体存在性）。
    if (contraptionId >= 0 &&
        contraptionState_.load(std::memory_order_relaxed) != 1) {
        removalsRejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::lock_guard<std::mutex> lock(blocksMutex_);
    // 裁决②：目标方块必须存在（原型线性查找，将来换坐标映射）。
    for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
        if (it->pos == p && it->contraptionId == contraptionId) {
            blocks_.erase(it);
            blocksRemoved_.fetch_add(1, std::memory_order_relaxed);
            // 自动排队 RemoveBlock 指令（下次 syncToPhysics 发出）。
            wantRemove_ = true;
            removePos_ = p;
            removeCid_ = contraptionId;
            return true;
        }
    }
    removalsRejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

int BusinessWorld::contraptionState() const {
    return contraptionState_.load(std::memory_order_relaxed);
}
double BusinessWorld::shipX() const { return shipX_.load(std::memory_order_relaxed); }
double BusinessWorld::shipY() const { return shipY_.load(std::memory_order_relaxed); }
double BusinessWorld::shipZ() const { return shipZ_.load(std::memory_order_relaxed); }
long long BusinessWorld::blocksPlaced() const {
    return blocksPlaced_.load(std::memory_order_relaxed);
}
long long BusinessWorld::blocksRemoved() const {
    return blocksRemoved_.load(std::memory_order_relaxed);
}
long long BusinessWorld::placementsRejected() const {
    return placementsRejected_.load(std::memory_order_relaxed);
}
long long BusinessWorld::removalsRejected() const {
    return removalsRejected_.load(std::memory_order_relaxed);
}
long long BusinessWorld::physicsBlockCount() const {
    return physicsBlockCount_.load(std::memory_order_relaxed);
}
long long BusinessWorld::collisions() const {
    return collisions_.load(std::memory_order_relaxed);
}

void BusinessWorld::maybeLog() {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - lastLog_).count() >= 1.0) {
        lastLog_ = now;
        const long long t = tickCount();
        const double sim = simTime_.load(std::memory_order_relaxed);
        const double tps = sim > 0.0 ? static_cast<double>(t) / sim : 0.0;
        std::ostringstream os;
        os << "[业务] sim=" << sim << "s tick=" << t
           << " 平均tps=" << tps
           << " 世界方块=" << blockCount()
           << " 物理碰撞体=" << physicsBlockCount()
           << " 放置/破坏=" << blocksPlaced() << "/" << blocksRemoved()
           << "(拒" << placementsRejected() << "/" << removalsRejected() << ")"
           << " 碰撞=" << collisions()
           << " 打包状态=" << contraptionState()
           << " 飞船=(" << shipX() << "," << shipY() << "," << shipZ() << ")"
           << "  生效系数=" << tm_.effectiveScale(engine::core::Domain::Business);
        engine::core::log(engine::core::LogLevel::Info, os.str());
    }
}

} // namespace engine::business

#include "business/PhysicsBridge.hpp"

namespace engine::business {

PhysicsBridge::PhysicsBridge() = default;

bool PhysicsBridge::queueSync() {
    // 请求全量同步：排队一条 SyncBlockSet（初始世界/世界重建时用）。
    // 返回 false 表示已有待发指令（原型：一次只排一条，防止覆盖）。
    if (wantSync_ || wantPlace_ || wantRemove_) return false;
    wantSync_ = true;
    return true;
}

void PhysicsBridge::queuePlace(const engine::shared::BlockPos& p, int type) {
    wantPlace_ = true;
    placePos_ = p;
    placeType_ = type;
}

void PhysicsBridge::queueRemove(const engine::shared::BlockPos& p) {
    wantRemove_ = true;
    removePos_ = p;
}

void PhysicsBridge::syncToPhysics(const std::vector<engine::shared::Block>& fullSnapshot) {
    // 业务 Tick 边界：下发指令。一次只发一条（原型节奏），
    // 优先级：全量同步 → 放置 → 破坏（MoveBlock 演示已移除，物理不再收到移动指令）。
    engine::shared::PhysicsCommand cmd;
    if (wantSync_) {
        // 全量同步：把业务世界所有方块（含类型/坐标）下发，物理重建静态碰撞体集合。
        // 区块化后从 chunks_ 现场生成全量列表（协议边界保持 vector<Block>）。
        cmd.type = engine::shared::PhysicsCommand::Type::SyncBlockSet;
        cmd.blocks = fullSnapshot;
        if (commandQueue_.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantSync_ = false;
        }
    } else if (wantPlace_) {
        // 放置指令：业务裁决已通过（位置合法），物理建静态碰撞体。
        cmd.type = engine::shared::PhysicsCommand::Type::PlaceBlock;
        cmd.x = placePos_.x; cmd.y = placePos_.y; cmd.z = placePos_.z;
        cmd.blockType = placeType_;
        cmd.contraptionId = -1; // 主世界静态方块（无刚体域）
        if (commandQueue_.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantPlace_ = false;
        }
    } else if (wantRemove_) {
        // 破坏指令：业务裁决已通过（方块存在），物理删静态碰撞体。
        cmd.type = engine::shared::PhysicsCommand::Type::RemoveBlock;
        cmd.x = removePos_.x; cmd.y = removePos_.y; cmd.z = removePos_.z;
        cmd.contraptionId = -1; // 主世界静态方块（无刚体域）
        if (commandQueue_.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantRemove_ = false;
        }
    }
    // push 失败（队列满）不清待发位：下 tick 重试。指令可延迟送达（最终一致）。
}

void PhysicsBridge::receiveFromPhysics() {
    // 业务 Tick 边界：收物理结果（位置快照/碰撞体数量）。
    engine::shared::PhysicsResult res;
    while (resultQueue_.pop(res)) {
        resultsReceived_.fetch_add(1, std::memory_order_relaxed);
        switch (res.type) {
        case engine::shared::PhysicsResult::Type::BlockMoved:
            blockX_.store(res.x, std::memory_order_relaxed);
            blockY_.store(res.y, std::memory_order_relaxed);
            blockZ_.store(res.z, std::memory_order_relaxed);
            break;
        case engine::shared::PhysicsResult::Type::BlockSetSize:
            // 物理静态碰撞体数量快照（合并语义：只留最新值）。
            physicsBlockCount_.store(res.blockSetSize, std::memory_order_relaxed);
            break;
        // 无刚体后物理不再发 CollisionEvent（碰撞事件只有刚体撞静态方块才有），
        // 收到也按未知类型忽略（switch 无匹配 case 直接跳过）。
        }
    }
}

engine::core::SPSCQueue<engine::shared::PhysicsCommand>& PhysicsBridge::commandQueue() {
    return commandQueue_;
}

engine::core::SPSCQueue<engine::shared::PhysicsResult>& PhysicsBridge::resultQueue() {
    return resultQueue_;
}

long long PhysicsBridge::commandsSent() const {
    return commandsSent_.load(std::memory_order_relaxed);
}
long long PhysicsBridge::resultsReceived() const {
    return resultsReceived_.load(std::memory_order_relaxed);
}
int PhysicsBridge::blockX() const { return blockX_.load(std::memory_order_relaxed); }
int PhysicsBridge::blockY() const { return blockY_.load(std::memory_order_relaxed); }
int PhysicsBridge::blockZ() const { return blockZ_.load(std::memory_order_relaxed); }
long long PhysicsBridge::physicsBlockCount() const {
    return physicsBlockCount_.load(std::memory_order_relaxed);
}

} // namespace engine::business

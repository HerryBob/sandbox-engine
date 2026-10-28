#include "business/BusinessWorld.hpp"
#include "core/Log.hpp"

#include <cmath>
#include <sstream>

namespace engine::business {

BusinessWorld::BusinessWorld(engine::core::TimeManager& tm,
                             PhysicsBridge& physicsBridge,
                             RenderBridge& renderBridge)
    : world_(),
      physicsBridge_(physicsBridge),
      renderBridge_(renderBridge),
      tm_(tm),
      startWall_(std::chrono::steady_clock::now()),
      lastLog_(std::chrono::steady_clock::now()) {
    // 初始世界（48×48 石头平台）由 World 构造函数创建（世界域职责）。
}

void BusinessWorld::advanceTick(double scale) {
    // 25TPS：每个业务 tick 推进 dt=0.04s，受 time_scale 缩放（与物理线程统一 25/25）。
    // simTime_ 是跨线程共享（main 线程读取），必须走原子读改写。
    // 本类只负责世界状态推进与裁决；事件由调用方在 tick 边界自行驱动。
    simTime_.store(simTime_.load(std::memory_order_relaxed) + 0.04 * scale,
                   std::memory_order_relaxed);
    ticks_.fetch_add(1, std::memory_order_relaxed);
    maybeLog();
}

bool BusinessWorld::syncAll() {
    // 请求全量同步：委托桥排队一条 SyncBlockSet（初始世界/世界重建时用）。
    // 返回 false 表示已有待发指令（原型：一次只排一条，防止覆盖）。
    return physicsBridge_.queueSync();
}

void BusinessWorld::syncToPhysics() {
    // tick 边界：委托 PhysicsBridge 发指令（全量同步需现场生成世界快照）。
    physicsBridge_.syncToPhysics(world_.renderSnapshot());
}

void BusinessWorld::receiveFromPhysics() {
    // tick 边界：委托 PhysicsBridge 收物理回执（位置快照/碰撞体数量）。
    physicsBridge_.receiveFromPhysics();
}

void BusinessWorld::flushChanges() {
    // tick 边界：委托 RenderBridge 发变更事件（事件源 = World 脏集合）。
    renderBridge_.flushChanges(world_, ticks_.load(std::memory_order_relaxed));
}
long long BusinessWorld::changeEventsSent() const {
    return renderBridge_.changeEventsSent();
}

double BusinessWorld::simTime() const {
    return simTime_.load(std::memory_order_relaxed);
}
long long BusinessWorld::tickCount() const {
    return ticks_.load(std::memory_order_relaxed);
}
long long BusinessWorld::commandsSent() const {
    return physicsBridge_.commandsSent();
}
long long BusinessWorld::resultsReceived() const {
    return physicsBridge_.resultsReceived();
}
int BusinessWorld::blockX() const { return physicsBridge_.blockX(); }
int BusinessWorld::blockY() const { return physicsBridge_.blockY(); }
int BusinessWorld::blockZ() const { return physicsBridge_.blockZ(); }

// ---- 查询：转发 World（数据真源在 World，锁在 World 内部）----
std::vector<engine::shared::Block> BusinessWorld::renderSnapshot() const {
    return world_.renderSnapshot();
}
std::size_t BusinessWorld::blockCount() const {
    return world_.blockCount();
}
std::size_t BusinessWorld::chunkCount() const {
    return world_.chunkCount();
}
void BusinessWorld::forEachBlockInAABB(
    const engine::shared::BlockPos& min, const engine::shared::BlockPos& max,
    const std::function<void(const engine::shared::BlockPos&,
                             engine::shared::BlockType)>& fn) const {
    world_.forEachBlockInAABB(min, max, fn);
}
std::uint32_t BusinessWorld::chunkDirtyFlags(const engine::shared::ChunkPos& cp) const {
    return world_.chunkDirtyFlags(cp);
}
std::optional<engine::shared::Block> BusinessWorld::findBlock
(const engine::shared::BlockPos& p) const {
    return world_.findBlock(p);
}

// ---- 动作：薄壳（裁决/写入/标脏在 World→base；排队指令委托 PhysicsBridge）----
bool BusinessWorld::placeBlock(const engine::shared::BlockPos& p,
                               engine::shared::BlockType type) {
    if (!world_.placeBlock(p, type)) {
        placementsRejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // 裁决通过 → 委托桥排队 PlaceBlock 指令（下次 syncToPhysics 发出）。
    physicsBridge_.queuePlace(p, static_cast<int>(type));
    blocksPlaced_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool BusinessWorld::removeBlock(const engine::shared::BlockPos& p) {
    if (!world_.removeBlock(p)) {
        removalsRejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // 裁决通过 → 委托桥排队 RemoveBlock 指令（下次 syncToPhysics 发出）。
    physicsBridge_.queueRemove(p);
    blocksRemoved_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

//// 玩家输入：DDA 射线检测 → 放置/破坏
// 相机来自 CameraState（输入线程写），点击来自 PlayerInput（鼠标回调写）。
void BusinessWorld::processPlayerInput(const engine::shared::CameraState& cam,
                                       const engine::shared::PlayerInput& clicks){
    const long long seq = clicks.clickSeq.load(std::memory_order_relaxed);
    if(seq == lastProcessedClickSeq_) return;
    lastProcessedClickSeq_ = seq;   //修复重复射线
    const int button = clicks.clickButton.load(std::memory_order_relaxed);

    //读取相机槽
    const double cx = cam.camX.load(std::memory_order_relaxed);
    const double cy = cam.camY.load(std::memory_order_relaxed);
    const double cz = cam.camZ.load(std::memory_order_relaxed);
    const double yaw = cam.yaw.load(std::memory_order_relaxed);
    const double pitch = cam.pitch.load(std::memory_order_relaxed);

    //实现方向必须和渲染端完全一致
    const double cosP = std::cos(pitch);
    const double dx = std::sin(yaw) * cosP;
    const double dy = std::sin(pitch);
    const double dz = -std::cos(yaw) * cosP;

    //DDA初始化
    const double ox = std::floor(cx), oy = std::floor(cy), oz = std::floor(cz);
    double tMaxX = (dx > 0 ? (ox + 1 - cx) / dx : (dx < 0 ? (ox - cx) / dx : 1e30));
    double tMaxY = (dy > 0 ? (oy + 1 - cy) / dy : (dy < 0 ? (oy - cy) / dy : 1e30));
    double tMaxZ = (dz > 0 ? (oz + 1 - cz) / dz : (dz < 0 ? (oz - cz) / dz : 1e30));
    const double tDeltaX = (dx != 0.0 ? std::abs(1.0 / dx) : 1e30);
    const double tDeltaY = (dy != 0.0 ? std::abs(1.0 / dy) : 1e30);
    const double tDeltaZ = (dz != 0.0 ? std::abs(1.0 / dz) : 1e30);
    const int stepX = (dx > 0 ? 1 : -1);
    const int stepY = (dy > 0 ? 1 : -1);
    const int stepZ = (dz > 0 ? 1 : -1);

    int ix = static_cast<int>(ox), iy = static_cast<int>(oy), iz = static_cast<int>(oz);
    int dirX = 0, dirY = 0, dirZ = 0;   // 进入命中格的推进方向（反推放置位用）
    bool hit = false;
    int hx = 0, hy = 0, hz = 0;         // 命中格（左键破坏目标）
    int px = ix, py = iy, pz = iz;      // 放置位（默认 = 起点格）
    
    // ⑤ DDA 推进：最多 8 格（射线距离上限）。每次选最小的 tMax 推进对应轴
    for (int s = 0; s < 8; ++s) {
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            tMaxX += tDeltaX; ix += stepX; dirX = stepX; dirY = 0; dirZ = 0;
        }
        else if (tMaxY < tMaxZ) {
            tMaxY += tDeltaY; iy += stepY; dirX = 0; dirY = stepY; dirZ = 0;
        }
        else {
            tMaxZ += tDeltaZ; iz += stepZ; dirX = 0; dirY = 0; dirZ = stepZ;
        }
        if (findBlock(engine::shared::BlockPos{ ix, iy, iz }).has_value()) {
            hit = true;
            hx = ix; hy = iy; hz = iz;
            px = ix - dirX; py = iy - dirY; pz = iz - dirZ;  // 放置位 = 命中格前一格
            break;
        }
    }

    playerActions_.fetch_add(1, std::memory_order_relaxed);
    if (!hit) return;  // 8 格内没碰到方块：点击已消费，无事可做

    if (button == 0) {  // 左键：破坏命中格
        removeBlock(engine::shared::BlockPos{ hx, hy, hz });
    }
    else if (button == 1) {  // 右键：在放置位放 Stone
        placeBlock(engine::shared::BlockPos{ px, py, pz },
            engine::shared::BlockType::Stone);
    }
}

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
    return physicsBridge_.physicsBlockCount();
}

void BusinessWorld::maybeLog() {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - lastLog_).count() >= 1.0) {
        lastLog_ = now;
        const long long t = tickCount();
        const double sim = simTime_.load(std::memory_order_relaxed);
        // tps 按墙钟口径（tick / 墙钟运行秒）：模拟时间受 time_scale 缩放，
        // 用 sim 做分母在加速/减速时会失真（加速时 tps 虚低、减速时虚高）。
        const double wall =
            std::chrono::duration<double>(now - startWall_).count();
        const double tps = wall > 0.0 ? static_cast<double>(t) / wall : 0.0;
        std::ostringstream os;
        os << "[业务] sim=" << sim << "s tick=" << t
           << " 平均tps=" << tps
           << " 世界方块=" << blockCount()
           << " 物理碰撞体=" << physicsBlockCount()
           << " 放置/破坏=" << blocksPlaced() << "/" << blocksRemoved()
           << "(拒" << placementsRejected() << "/" << removalsRejected() << ")"
           << "  生效系数=" << tm_.effectiveScale(engine::core::Domain::Business);
        engine::core::log(engine::core::LogLevel::Info, os.str());
    }
}

} // namespace engine::business

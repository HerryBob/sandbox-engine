#include "business/BusinessWorld.hpp"
#include "core/Log.hpp"
#include "registry/BlockRegistry.hpp"

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
    // 初始世界：由装配层（main）用 ColumnGenerator + ChunkStreamer 从数据预设生成
    // （World 构造函数不再预置方块）。
}

void BusinessWorld::tick(double scale) {
    // 首个业务 tick 强制一次全量同步：地形预热在装配期（主线程）完成，
    // 业务线程起来后没有新列可触发 requestPhysicsResync，物理侧会一直空集合。
    if (firstTick_.load(std::memory_order_relaxed)) {
        firstTick_.store(false, std::memory_order_relaxed);
        resyncPending_.store(true, std::memory_order_relaxed);
    }
    // 25TPS：每个业务 tick 推进 dt=0.04s，受 time_scale 缩放（与物理线程统一 25/25）。
    // simTime_ 是跨线程共享（main 线程读取），必须走原子读改写。
    // 本类只负责世界状态推进与裁决；事件由调用方在 tick 边界自行驱动。
    // P8：C++20 的 atomic<double>::fetch_add —— 原来 store(load()+x) 是两次原子操作，
    // 读改写之间可能被别的读者看到中间态。
    simTime_.fetch_add(0.04 * scale, std::memory_order_relaxed);
    tickCount_.fetch_add(1, std::memory_order_relaxed);
    maybeLog();
}

bool BusinessWorld::syncAll() {
    // 请求全量同步：委托桥排队一条 SyncBlockSet（初始世界/世界重建时用）。
    // 返回 false 表示已有待发指令（原型：一次只排一条，防止覆盖）。
    return physicsBridge_.pushSyncCommand();
}

void BusinessWorld::syncWorldToPhysics() {
    // 区块卸载等批量变更后需要重发全量：指令槽位是单槽位状态机，
    // pushSyncCommand 在"已有待发指令"时返回 false —— 保留标志，下 tick 继续重试，
    // 否则物理侧会一直留着已卸载区域的幽灵碰撞体。
    if (resyncPending_.load(std::memory_order_relaxed)) {
        if (physicsBridge_.pushSyncCommand()) resyncPending_.store(false, std::memory_order_relaxed);
    }
    // tick 边界：委托 PhysicsBridge 发指令。
    //
    // P1（性能）：**只有真的待发全量同步时才拍快照**。
    // 原来这里无条件 `world_.renderSnapshot()`：25 次/秒 × 全量持锁拷贝。
    // 实测 82,944 方块时每秒 25~26 次、约 33 MB/s 的纯拷贝，稳态下全是白做；
    // 10k chunk 时单次需几百 ms、峰值内存近 1GB（OOM 直接崩）。
    // 现在稳态快照次数为 0，只有 chunk 加载/卸载触发 resync 的那几个 tick 才拍。
    static const std::vector<engine::shared::Block> kEmptySnapshot; // 无同步时不构造临时表
    if (physicsBridge_.hasPendingSync()) {
        ++syncSnapshotCount_;   // 诊断：1Hz 日志里的"全量快照=N/s"
        physicsBridge_.syncWorldToPhysics(world_.renderSnapshot());
    } else {
        physicsBridge_.syncWorldToPhysics(kEmptySnapshot);
    }
}

void BusinessWorld::receiveResultsFromPhysics() {
    // tick 边界：委托 PhysicsBridge 收物理回执（位置快照/碰撞体数量/刚体数据）。
    // B96：复用成员而不是每 tick 构造局部 vector —— 它只在业务线程用。
    bodyEvents_.clear();
    std::optional<engine::shared::BodyPose> bodyPose;
    physicsBridge_.receiveResultsFromPhysics(&bodyEvents_, &bodyPose);
    // 刚体数据转给渲染桥：结构事件逐条送（事件流），位姿只送最新（状态）。
    // push 失败即逻辑异常（队列已从 16 提到 64），不在此处重试。
    // B95：**检查返回值** —— 事件流丢了会导致渲染端多一个/漏一个网格，
    // 以前完全静默（连日志都没有）。
    for (const auto& ev : bodyEvents_) {
        if (!renderBridge_.pushBodyStructure(ev)) {
            ENGINE_LOG_ERROR("Render",
                "刚体结构事件入队失败（队列满）：contraptionId=" +
                std::to_string(ev.contraptionId) + "，渲染端会缺/多一个网格");
        }
    }
    // 刚体位姿不再转发给渲染：渲染端直接读 PoseHistory（物理每 tick 发布）。
    // 原来这一跳（物理→结果队列→业务→渲染队列）多出 ~40ms 延迟且到达抖动大。
    // bodyPose 仍由 receiveResultsFromPhysics 产出，留给业务侧将来可能需要的位姿查询。
    (void)bodyPose;
}

void BusinessWorld::flushChangesToRender() {
    // tick 边界：委托 RenderBridge 发变更事件（事件源 = World 脏集合）。
    renderBridge_.flushChangesToRender(world_, tickCount_.load(std::memory_order_relaxed));
}
long long BusinessWorld::sentChangeEventCount() const {
    return renderBridge_.sentChangeEventCount();
}

double BusinessWorld::simTime() const {
    return simTime_.load(std::memory_order_relaxed);
}
long long BusinessWorld::tickCount() const {
    return tickCount_.load(std::memory_order_relaxed);
}
long long BusinessWorld::sentCommandCount() const {
    return physicsBridge_.sentCommandCount();
}
long long BusinessWorld::receivedResultCount() const {
    return physicsBridge_.receivedResultCount();
}
int BusinessWorld::blockX() const { return physicsBridge_.blockX(); }
int BusinessWorld::blockY() const { return physicsBridge_.blockY(); }
int BusinessWorld::blockZ() const { return physicsBridge_.blockZ(); }

// ---- 查询：转发 World（数据真源在 World，锁在 World 内部）----
std::vector<engine::shared::Block> BusinessWorld::renderSnapshot() const {
    return world_.renderSnapshot();
}
std::vector<engine::shared::Block> BusinessWorld::chunkSnapshot(
    const std::unordered_set<engine::shared::ChunkPos>& chunks,
    bool expandNeighbors) const {
    return world_.chunkSnapshot(chunks, expandNeighbors);
}
std::size_t BusinessWorld::blockCount() const {
    return world_.blockCount();
}
std::size_t BusinessWorld::chunkCount() const {
    return world_.chunkCount();
}
std::optional<engine::shared::Block> BusinessWorld::findBlock
(const engine::shared::BlockPos& p) const {
    return world_.findBlock(p);
}

// ---- 动作：薄壳（裁决/写入/标脏在 World→base；排队指令委托 PhysicsBridge）----
bool BusinessWorld::placeBlock(const engine::shared::BlockPos& p,
                               engine::shared::BlockType type) {
    if (!world_.placeBlock(p, type)) {
        rejectedPlacementCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // 裁决通过 → 委托桥排队 PlaceBlock 指令（下次 syncWorldToPhysics 发出）。
    physicsBridge_.pushPlaceCommand(p, static_cast<int>(type));
    placedBlockCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool BusinessWorld::removeBlock(const engine::shared::BlockPos& p) {
    if (!world_.removeBlock(p)) {
        rejectedRemovalCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // 裁决通过 → 委托桥排队 RemoveBlock 指令（下次 syncWorldToPhysics 发出）。
    physicsBridge_.pushRemoveCommand(p);
    removedBlockCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

//// 玩家输入：逐条消费点击队列 → DDA 射线检测 → 放置/破坏
// 相机来自 CameraState（渲染线程写），点击来自 PlayerInput 的 SPSC 队列（鼠标回调 push）。
void BusinessWorld::handlePlayerInput(const engine::shared::CameraState& cam,
                                       engine::shared::PlayerInput& clicks){
    // 相机槽（渲染线程写）在本次 tick 内只读一次：同一 tick 的多次点击共享同一视角。
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

    const double ox = std::floor(cx), oy = std::floor(cy), oz = std::floor(cz);
    const double tDeltaX = (dx != 0.0 ? std::abs(1.0 / dx) : 1e30);
    const double tDeltaY = (dy != 0.0 ? std::abs(1.0 / dy) : 1e30);
    const double tDeltaZ = (dz != 0.0 ? std::abs(1.0 / dz) : 1e30);
    const int stepX = (dx > 0 ? 1 : -1);
    const int stepY = (dy > 0 ? 1 : -1);
    const int stepZ = (dz > 0 ? 1 : -1);

    // 逐条消费：一个 tick 内落进几次点击就处理几次。
    // 旧实现用"序号 + 按钮"两个原子变量、业务侧按序号跳号，一个 40ms tick 内的第二次点击会被吞掉。
    engine::shared::ClickEvent ev;
    while (clicks.clicks.pop(ev)) {
        // DDA 状态每次点击独立 → 在循环内初始化
        double tMaxX = (dx > 0 ? (ox + 1 - cx) / dx : (dx < 0 ? (ox - cx) / dx : 1e30));
        double tMaxY = (dy > 0 ? (oy + 1 - cy) / dy : (dy < 0 ? (oy - cy) / dy : 1e30));
        double tMaxZ = (dz > 0 ? (oz + 1 - cz) / dz : (dz < 0 ? (oz - cz) / dz : 1e30));

        int ix = static_cast<int>(ox), iy = static_cast<int>(oy), iz = static_cast<int>(oz);
        int dirX = 0, dirY = 0, dirZ = 0;   // 进入命中格的推进方向（反推放置位用）
        bool hit = false;
        int hx = 0, hy = 0, hz = 0;         // 命中格（左键破坏目标）
        int px = ix, py = iy, pz = iz;      // 放置位（默认 = 起点格）

        // B101：**起点格也参与判定**。原来是"先推进、再检查"，所以站在方块里时
        // 打不到身上这一格（射线第一格永远是邻格）。先探一次起点，再决定要不要推进。
        {
            engine::shared::BlockType t0 = engine::shared::BlockType::Air;
            if (lookupBlock(engine::shared::BlockPos{ ix, iy, iz }, t0) ==
                World::LookupResult::Occupied) {
                hit = true; hx = ix; hy = iy; hz = iz;
                px = ix; py = iy; pz = iz;  // 无"前格"：放置位退化为自身（会被裁决拒绝）
            }
        }

        // ⑤ DDA 推进：最多 8 格（射线距离上限）。每次选最小的 tMax 推进对应轴
        for (int s = 0; s < 8 && !hit; ++s) {
            if (tMaxX < tMaxY && tMaxX < tMaxZ) {
                tMaxX += tDeltaX; ix += stepX; dirX = stepX; dirY = 0; dirZ = 0;
            }
            else if (tMaxY < tMaxZ) {
                tMaxY += tDeltaY; iy += stepY; dirX = 0; dirY = stepY; dirZ = 0;
            }
            else {
                tMaxZ += tDeltaZ; iz += stepZ; dirX = 0; dirY = 0; dirZ = stepZ;
            }
            // B97：改用三态查询。原来用 findBlock（两态），把"所在 chunk 未加载"
            // 和"这里是空气"当成了同一件事 —— 射线会直接穿过未加载区域，
            // 命中背后的方块、或者算出很远的放置位，表现为"放块位置随机"。
            // 现在遇到未加载就结束射线：宁可这一下不生效，也不猜。
            engine::shared::BlockType hitType = engine::shared::BlockType::Air;
            const auto q = lookupBlock(engine::shared::BlockPos{ ix, iy, iz }, hitType);
            if (q == World::LookupResult::NotLoaded) break;   // 未加载：射线到此为止
            if (q == World::LookupResult::Occupied) {
                hit = true;
                hx = ix; hy = iy; hz = iz;
                px = ix - dirX; py = iy - dirY; pz = iz - dirZ;  // 放置位 = 命中格前一格
                break;
            }
        }

        playerActions_.fetch_add(1, std::memory_order_relaxed);
        if (!hit) continue;  // 8 格内没碰到方块：本次点击已消费，无事可做

        if (ev.button == 0) {  // 左键：破坏命中格
            // B99：检查返回值 —— 失败说明裁决拒绝（方块已不存在），以前完全静默。
            if (!removeBlock(engine::shared::BlockPos{ hx, hy, hz })) {
                ENGINE_LOG_DEBUG("Business", "破坏被拒绝（目标已不是方块）");
            }
        }
        else if (ev.button == 1) {  // 右键：在放置位放 Stone
            // 方块 id 由注册表运行期分配，不能写死枚举值——读 freeze 后解析好的缓存。
            // B99：同上，放置失败（位置已占用）以前也是静默的。
            if (!placeBlock(engine::shared::BlockPos{ px, py, pz },
                    static_cast<engine::shared::BlockType>(
                        engine::registry::g_builtinBlockIds.stone))) {
                ENGINE_LOG_DEBUG("Business", "放置被拒绝（位置已占用）");
            }
        }
    }

    // F9：把准星对着的那一个方块打包成刚体（仅第一人称；见 handlePackInFront）
    if (clicks.packInFront.exchange(false, std::memory_order_relaxed)) {
        handlePackInFront(cam);
    }
}

// F9：把准星对着的那一个方块打包成刚体（仅第一人称）。
//
// 为什么单独写一遍射线（没复用 handlePlayerInput 里的 DDA）：
// 那段 DDA 与"点击事件 / 放置位"耦合在一个大循环里，直接抽函数的风险高于收益。
// 这里保留同构的数学；后续统一时应合并（TODO）。
void BusinessWorld::handlePackInFront(const engine::shared::CameraState& cam) {
    // 只在第一人称生效：自由相机/第三人称下准星不代表玩家视线，避免误打包
    if (cam.viewMode.load(std::memory_order_relaxed) != 1) {
        ENGINE_LOG_DEBUG("Business", "F9 打包被忽略（非第一人称）");
        return;
    }

    const double cx = cam.camX.load(std::memory_order_relaxed);
    const double cy = cam.camY.load(std::memory_order_relaxed);
    const double cz = cam.camZ.load(std::memory_order_relaxed);
    const double yaw = cam.yaw.load(std::memory_order_relaxed);
    const double pitch = cam.pitch.load(std::memory_order_relaxed);

    // 方向必须与渲染端 / 点击射线完全一致（否则"看着对准、实际打空"）
    const double cosP = std::cos(pitch);
    const double dx = std::sin(yaw) * cosP;
    const double dy = std::sin(pitch);
    const double dz = -std::cos(yaw) * cosP;

    const double ox = std::floor(cx), oy = std::floor(cy), oz = std::floor(cz);
    const double tDeltaX = (dx != 0.0 ? std::abs(1.0 / dx) : 1e30);
    const double tDeltaY = (dy != 0.0 ? std::abs(1.0 / dy) : 1e30);
    const double tDeltaZ = (dz != 0.0 ? std::abs(1.0 / dz) : 1e30);
    const int stepX = (dx > 0 ? 1 : -1);
    const int stepY = (dy > 0 ? 1 : -1);
    const int stepZ = (dz > 0 ? 1 : -1);
    double tMaxX = (dx > 0 ? (ox + 1 - cx) / dx : (dx < 0 ? (ox - cx) / dx : 1e30));
    double tMaxY = (dy > 0 ? (oy + 1 - cy) / dy : (dy < 0 ? (oy - cy) / dy : 1e30));
    double tMaxZ = (dz > 0 ? (oz + 1 - cz) / dz : (dz < 0 ? (oz - cz) / dz : 1e30));

    int ix = static_cast<int>(ox), iy = static_cast<int>(oy), iz = static_cast<int>(oz);
    constexpr int kPackRaySteps = 8;

    // 起点格直接是方块（站在方块里）→ 不动手：否则会把脚下的地打包成刚体，玩家掉坑
    {
        engine::shared::BlockType t0 = engine::shared::BlockType::Air;
        if (lookupBlock(engine::shared::BlockPos{ix, iy, iz}, t0) == World::LookupResult::Occupied) {
            return;
        }
    }

    for (int s = 0; s < kPackRaySteps; ++s) {
        if (tMaxX < tMaxY && tMaxX < tMaxZ) { tMaxX += tDeltaX; ix += stepX; }
        else if (tMaxY < tMaxZ)             { tMaxY += tDeltaY; iy += stepY; }
        else                                 { tMaxZ += tDeltaZ; iz += stepZ; }

        engine::shared::BlockType hitType = engine::shared::BlockType::Air;
        const auto q = lookupBlock(engine::shared::BlockPos{ix, iy, iz}, hitType);
        if (q == World::LookupResult::NotLoaded) return;   // 未加载就停（不猜）
        if (q != World::LookupResult::Occupied) continue;

        const engine::shared::BlockPos hitPos{ix, iy, iz};
        // 先从世界删掉（渲染与存档跟着变）。物理侧**不发 RemoveBlock**：
        // PackContraption 自带"碰撞体从静态集合转移到刚体"（onPackContraption 里 erase）。
        if (!world_.removeBlock(hitPos)) return;

        engine::shared::PhysicsCommand pack;
        pack.type = engine::shared::PhysicsCommand::Type::PackContraption;
        pack.contraptionId = 1;   // 占位：物理侧会分配真正的结构 id
        pack.blocks.push_back(engine::shared::Block{hitType, hitPos, -1});
        pack.blockCount = 1;
        if (!physicsBridge_.businessToPhysicsCommands().push(std::move(pack))) {
            // 队列满：把方块放回去，避免"世界里凭空少一块"
            world_.placeBlock(hitPos, hitType);
            ENGINE_LOG_WARN("Business", "F9 打包失败（指令队列满），方块已还原");
            return;
        }
        ENGINE_LOG_INFO("Business", "F9 打包：pos=(" + std::to_string(hitPos.x) + "," +
            std::to_string(hitPos.y) + "," + std::to_string(hitPos.z) + ")");
        return;
    }
}

void BusinessWorld::requestPhysicsResync() {    // 只置标志；真正的下发在 syncWorldToPhysics（那里才有世界快照，且每 tick 会重试）。
    resyncPending_.store(true, std::memory_order_relaxed);
}

// 增量同步：直接把一条 SyncChunks 推进队列（不经过"待发快照"那条路，避免又拍一次全量）。
void BusinessWorld::syncChunksToPhysics(std::vector<engine::shared::Block> blocks,
                                        std::vector<engine::shared::ChunkPos> chunks) {
    if (chunks.empty()) return;
    engine::shared::PhysicsCommand cmd;
    cmd.type = engine::shared::PhysicsCommand::Type::SyncChunks;
    cmd.blocks = std::move(blocks);
    cmd.changedChunks = std::move(chunks);
    if (!physicsBridge_.businessToPhysicsCommands().push(std::move(cmd))) {
        // 队列满：退回全量（下次 syncWorldToPhysics 会重发整个世界），保证最终一致
        resyncPending_.store(true, std::memory_order_relaxed);
    }
}

long long BusinessWorld::placedBlockCount() const {
    return placedBlockCount_.load(std::memory_order_relaxed);
}
long long BusinessWorld::removedBlockCount() const {
    return removedBlockCount_.load(std::memory_order_relaxed);
}
long long BusinessWorld::rejectedPlacementCount() const {
    return rejectedPlacementCount_.load(std::memory_order_relaxed);
}
long long BusinessWorld::rejectedRemovalCount() const {
    return rejectedRemovalCount_.load(std::memory_order_relaxed);
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
        // 诊断（P1）：上一秒拍了几次全量快照。stead-state 应为 0 ——
        // 因为只有"待发全量同步"时才需要快照；每 tick 都拍就是 P1 描述的问题。
        const long long snapsThisSecond = syncSnapshotCount_ - lastLogSnapshotCount_;
        lastLogSnapshotCount_ = syncSnapshotCount_;
        std::ostringstream os;
        os << "[业务] sim=" << sim << "s tick=" << t
           << " 平均tps=" << tps
           << " 全量快照=" << snapsThisSecond << "/s"
           << " 世界方块=" << blockCount()
           << " 物理碰撞体=" << physicsBlockCount()
           << " 放置/破坏=" << placedBlockCount() << "/" << removedBlockCount()
           << "(拒" << rejectedPlacementCount() << "/" << rejectedRemovalCount() << ")"
           << "  生效系数=" << tm_.scale();
        ENGINE_LOG_INFO("Business", os.str());
    }
}

} // namespace engine::business

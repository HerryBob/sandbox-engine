#include "business/BusinessWorld.hpp"

#include "core/Log.hpp"
#include "registry/BlockRegistry.hpp"
#include "shared/block/StructureFrame.hpp"
#include "shared/pick/StructureRay.hpp"

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

    // ---- 结构镜像：方块来自结构事件，位姿来自 RigidBodyPos 回执 ----
    // 用途只有一个：让"这次点击该打谁"在业务线程同步算出来（不再跨线程问物理）。
    // 物理侧仍是形状的权威 —— 编辑指令下去后它会自己重新求交。
    for (const auto& ev : bodyEvents_) {
        if (ev.destroy) {
            structures_.erase(ev.contraptionId);
        } else {
            StructureMirror& m = structures_[ev.contraptionId];
            m.blocks = ev.blocks;
            rebuildStructureIndex(m);
        }
    }
    for (const auto& pose : physicsBridge_.bodyPoses()) {
        auto it = structures_.find(pose.contraptionId);
        if (it == structures_.end()) continue;   // 只有位姿、还没收到方块：等下一帧
        it->second.cx = pose.x;
        it->second.cy = pose.y;
        it->second.cz = pose.z;
        it->second.rot = glm::dquat(pose.qw, pose.qx, pose.qy, pose.qz);
        it->second.hasPose = true;
    }
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


    // 逐条消费：一个 tick 内落进几次点击就处理几次。
    // 每次点击**同步**完成：体素射线（查 World）+ 刚体射线（查结构镜像）→ 裁决 → 执行。
    // 不再跨线程问物理，所以方块操作没有额外延迟（刚体改形状仍要等物理 tick，那是它自己的职责）。
    engine::shared::ClickEvent ev;
    while (clicks.clicks.pop(ev)) {
        engine::shared::pick::PickRay ray;
        ray.ox = cx; ray.oy = cy; ray.oz = cz;
        ray.dx = dx; ray.dy = dy; ray.dz = dz;
        ray.maxDist = 8.0;

        using namespace engine::shared::pick;
        const PickHit blockHit = raycastVoxel(ray);
        const PickHit bodyHit = raycastStructures(ray);
        const PickAction action = resolve(blockHit, bodyHit, /*bodyProbeTimedOut=*/false,
            ev.button == 0,
            static_cast<std::uint16_t>(engine::registry::g_builtinBlockIds.stone));
        if (action.kind == PickActionKind::None) {
            ENGINE_LOG_DEBUG("Business", "拾取无动作（两条射线都没命中可用目标）");
        }
        applyPick(ray, action);
    }
}

// ---- 鼠标拾取：体素射线 + 刚体探测 → 裁决 → 唯一动作 ----
//
// 为什么体素射线单独实现一份：原来它内联在 handlePlayerInput 的点击循环里，
// 与"放置位反推"耦合；现在点击要先算体素结果、再等物理回执，必须能独立调用。
// 数学与旧实现完全一致（含 B97 未加载即停、B101 起点格参与判定）。
engine::shared::pick::PickHit BusinessWorld::raycastVoxel(
    const engine::shared::pick::PickRay& ray) const {
    using engine::shared::pick::PickHit;
    using engine::shared::pick::HitKind;

    PickHit out;
    const double cx = ray.ox, cy = ray.oy, cz = ray.oz;
    const double dx = ray.dx, dy = ray.dy, dz = ray.dz;
    // 起点格（floor）与各轴步进量：与旧内联 DDA 完全相同的数学
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
    int dirX = 0, dirY = 0, dirZ = 0;   // 进入命中格的推进方向（反推放置位用）

    // B101：起点格也参与判定（站在方块里时能打到身上这一格）
    {
        engine::shared::BlockType t0 = engine::shared::BlockType::Air;
        if (lookupBlock(engine::shared::BlockPos{ ix, iy, iz }, t0) ==
            World::LookupResult::Occupied) {
            out.kind = HitKind::Block;
            out.t = 0.0;
            out.bx = ix; out.by = iy; out.bz = iz;
            out.px = ix; out.py = iy; out.pz = iz;   // 无前格：放置位退化为自身（会被拒）
            out.blockType = static_cast<std::uint16_t>(t0);
            return out;
        }
    }

    // DDA 推进：最多 8 格
    double tEntry = 0.0;
    for (int s = 0; s < 8; ++s) {
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            tEntry = tMaxX; tMaxX += tDeltaX;
            ix += stepX; dirX = stepX; dirY = 0; dirZ = 0;
        } else if (tMaxY < tMaxZ) {
            tEntry = tMaxY; tMaxY += tDeltaY;
            iy += stepY; dirX = 0; dirY = stepY; dirZ = 0;
        } else {
            tEntry = tMaxZ; tMaxZ += tDeltaZ;
            iz += stepZ; dirX = 0; dirY = 0; dirZ = stepZ;
        }
        if (tEntry > ray.maxDist) break;   // 超出射线长度
        // B97：三态查询 —— 遇到未加载就结束射线（宁可这一下不生效，也不猜）
        engine::shared::BlockType hitType = engine::shared::BlockType::Air;
        const auto q = lookupBlock(engine::shared::BlockPos{ ix, iy, iz }, hitType);
        if (q == World::LookupResult::NotLoaded) break;
        if (q == World::LookupResult::Occupied) {
            out.kind = HitKind::Block;
            out.t = tEntry;
            out.bx = ix; out.by = iy; out.bz = iz;
            out.px = ix - dirX; out.py = iy - dirY; out.pz = iz - dirZ;
            out.blockType = static_cast<std::uint16_t>(hitType);
            return out;
        }
    }
    return out;   // 未命中：kind 保持 None，t = 1e30（裁决时视为"更远"）
}

// 发一条刚体编辑指令（裁决之后才发；物理侧重新求交并改形状）。
// 注意：业务侧已经用镜像算过命中，这里只是"请物理照做"——
// 物理侧仍会用它自己的射线重算一遍（它才是形状的权威），所以镜像即使短暂不同步也不会写坏状态。
void BusinessWorld::sendStructureInteract(const engine::shared::pick::PickRay& ray, int action) {
    engine::shared::PhysicsCommand cmd;
    cmd.type = engine::shared::PhysicsCommand::Type::InteractStructure;
    cmd.rox = ray.ox; cmd.roy = ray.oy; cmd.roz = ray.oz;
    cmd.rdx = ray.dx; cmd.rdy = ray.dy; cmd.rdz = ray.dz;
    cmd.rmaxDist = ray.maxDist;
    cmd.raction = action;
    cmd.blockType = static_cast<int>(engine::registry::g_builtinBlockIds.stone);
    if (!physicsBridge_.pushInteractCommand(cmd)) {
        ENGINE_LOG_WARN("Business", "刚体交互指令队列满，本次忽略");
    }
}

// ---- 结构镜像维护 ----
void BusinessWorld::rebuildStructureIndex(StructureMirror& m) {
    m.occupied.clear();
    m.occupied.reserve(m.blocks.size() * 2);
    for (const auto& b : m.blocks) {
        m.occupied.insert(posKey(b.pos.x, b.pos.y, b.pos.z));
    }
    // 质心偏置：与物理侧同一份实现（shared/block/StructureFrame.hpp），
    // 所以两边算出的"格点原点"必然一致。
    m.bias = engine::shared::structureBias(m.blocks);
}

// 刚体射线：每个结构把射线变到局部空间做体素 DDA，取最近命中。
engine::shared::pick::PickHit BusinessWorld::raycastStructures(
    const engine::shared::pick::PickRay& ray) const {
    using namespace engine::shared::pick;
    PickHit out;   // kind=None, t=1e30

    for (const auto& [id, m] : structures_) {
        if (!m.hasPose || m.blocks.empty()) continue;

        // 世界 → 局部：先减 anchor（= COM − R·bias，与 physics 发布位姿的语义一致），再反转旋转
        const glm::dquat invRot = glm::conjugate(m.rot);
        const glm::dvec3 com(m.cx, m.cy, m.cz);
        const glm::dvec3 anchor = com - m.rot * m.bias;
        const glm::dvec3 rel(ray.ox - anchor.x, ray.oy - anchor.y, ray.oz - anchor.z);
        const glm::dvec3 lo = invRot * rel;
        const glm::dvec3 ld = invRot * glm::dvec3(ray.dx, ray.dy, ray.dz);

        // 整结构 AABB 快速剔除（局部坐标下是整数格的范围）
        double mnx = 1e30, mxx = -1e30, mny = 1e30, mxy = -1e30, mnz = 1e30, mxz = -1e30;
        for (const auto& b : m.blocks) {
            mnx = std::min(mnx, static_cast<double>(b.pos.x)); mxx = std::max(mxx, static_cast<double>(b.pos.x) + 1.0);
            mny = std::min(mny, static_cast<double>(b.pos.y)); mxy = std::max(mxy, static_cast<double>(b.pos.y) + 1.0);
            mnz = std::min(mnz, static_cast<double>(b.pos.z)); mxz = std::max(mxz, static_cast<double>(b.pos.z) + 1.0);
        }
        {
            double t0 = 0.0, t1 = ray.maxDist;
            const double dirs[3] = { ld.x, ld.y, ld.z };
            const double os[3] = { lo.x, lo.y, lo.z };
            const double mins[3] = { mnx, mny, mnz };
            const double maxs[3] = { mxx, mxy, mxz };
            bool miss = false;
            for (int a = 0; a < 3; ++a) {
                if (std::fabs(dirs[a]) < 1e-12) {
                    if (os[a] < mins[a] || os[a] > maxs[a]) { miss = true; break; }
                    continue;
                }
                double ta = (mins[a] - os[a]) / dirs[a];
                double tb = (maxs[a] - os[a]) / dirs[a];
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
                if (t0 > t1) { miss = true; break; }
            }
            if (miss) continue;
        }

        const LocalHit h = raycastLocalVoxel(lo.x, lo.y, lo.z, ld.x, ld.y, ld.z, ray.maxDist,
            [&m](int x, int y, int z) {
                return m.occupied.find(posKey(x, y, z)) != m.occupied.end();
            });
        if (!h.hit) continue;
        if (h.t < out.t) {
            out.kind = HitKind::Body;
            out.t = h.t;
            out.structureId = id;
            out.lx = h.x; out.ly = h.y; out.lz = h.z;
            out.nx = h.nx; out.ny = h.ny; out.nz = h.nz;
        }
    }
    return out;
}

// 执行裁决结果：方块走 World（同步），刚体发指令给物理（下一物理 tick 生效）。
void BusinessWorld::applyPick(const engine::shared::pick::PickRay& ray,
                              const engine::shared::pick::PickAction& a) {
    using engine::shared::pick::PickActionKind;
    switch (a.kind) {
    case PickActionKind::BreakBlock:
        if (!removeBlock(engine::shared::BlockPos{ a.block.bx, a.block.by, a.block.bz })) {
            ENGINE_LOG_DEBUG("Business", "破坏被拒绝（目标已不是方块）");
        }
        break;
    case PickActionKind::PlaceBlock:
        // 方块 id 由注册表运行期分配，不能写死枚举值 —— 用 freeze 后解析好的缓存
        if (!placeBlock(engine::shared::BlockPos{ a.block.px, a.block.py, a.block.pz },
                static_cast<engine::shared::BlockType>(a.block.blockType))) {
            ENGINE_LOG_DEBUG("Business", "放置被拒绝（位置已占用）");
        }
        break;
    case PickActionKind::BreakBodyBlock:
        sendStructureInteract(ray, 0);
        break;
    case PickActionKind::PlaceBodyBlock:
        sendStructureInteract(ray, 1);
        break;
    case PickActionKind::None:
    default:
        break;
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

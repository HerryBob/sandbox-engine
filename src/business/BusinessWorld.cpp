#include "business/BusinessWorld.hpp"
#include "core/Log.hpp"

#include <cmath>
#include <sstream>

namespace engine::business {

BusinessWorld::BusinessWorld(engine::core::TimeManager& tm)
    : tm_(tm),
      startWall_(std::chrono::steady_clock::now()),
      lastLog_(std::chrono::steady_clock::now()) {
    // 初始世界：16×16 石头平台（x,z ∈ [-8,7]，y=0，占 [0,1] 顶面 y=1，玩家站上面）。
    // 数据层 ② 区块化：方块写入 16³ chunk（chunks_ 稀疏映射，空 chunk 不占内存）。
    auto setBlock = [&](int x, int y, int z, engine::shared::BlockType t) {
        const engine::shared::BlockPos p{x, y, z};
        auto& c = chunks_[engine::shared::chunkOf(p)]; // operator[] 缺省构造新 chunk
        c.atWorld(p) = static_cast<std::uint8_t>(t);
        c.markDirty(0x01); // 修改跟踪：初始装载标记脏（变更事件流种子）
    };
    for (int x = -24; x <= 23; ++x)
        for (int z = -24; z <= 23; ++z)
            setBlock(x, 0, z, engine::shared::BlockType::Stone);
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
    // 请求全量同步：排队一条 SyncBlockSet（初始世界/世界重建时用）。
    // 返回 false 表示已有待发指令（原型：一次只排一条，防止覆盖）。
    if (wantSync_ || wantPlace_ || wantRemove_) return false;
    wantSync_ = true;
    return true;
}

void BusinessWorld::syncToPhysics(engine::core::SPSCQueue<engine::shared::PhysicsCommand>& q) {
    // 业务 Tick 边界：下发指令。一次只发一条（原型节奏），
    // 优先级：全量同步 → 放置 → 破坏（MoveBlock 演示已移除，物理不再收到移动指令）。
    engine::shared::PhysicsCommand cmd;
    if (wantSync_) {
        // 全量同步：把业务世界所有方块（含类型/坐标）下发，物理重建静态碰撞体集合。
        // 区块化后从 chunks_ + bodyBlocks_ 现场生成全量列表（协议边界保持 vector<Block>）。
        cmd.type = engine::shared::PhysicsCommand::Type::SyncBlockSet;
        cmd.blocks = renderSnapshot();
        if (q.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantSync_ = false;
        }
    } else if (wantPlace_) {
        // 放置指令：业务裁决已通过（位置合法），物理建静态碰撞体。
        cmd.type = engine::shared::PhysicsCommand::Type::PlaceBlock;
        cmd.x = placePos_.x; cmd.y = placePos_.y; cmd.z = placePos_.z;
        cmd.blockType = placeType_;
        cmd.contraptionId = -1; // 主世界静态方块（无刚体域）
        if (q.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantPlace_ = false;
        }
    } else if (wantRemove_) {
        // 破坏指令：业务裁决已通过（方块存在），物理删静态碰撞体。
        cmd.type = engine::shared::PhysicsCommand::Type::RemoveBlock;
        cmd.x = removePos_.x; cmd.y = removePos_.y; cmd.z = removePos_.z;
        cmd.contraptionId = -1; // 主世界静态方块（无刚体域）
        if (q.push(cmd)) {
            commandsSent_.fetch_add(1, std::memory_order_relaxed);
            wantRemove_ = false;
        }
    } 
}

void BusinessWorld::flushChanges(
    engine::core::SPSCQueue<engine::shared::BlockChangeEvent>& q) {
    // 业务 Tick 边界：把本 tick 变脏的 chunk 聚合为一条变更事件发出（数据层 ② 第四项）。
    // 事件源 = dirtyChunksThisTick_（写操作插入，自动去重）；发完清空。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    if (dirtyChunksThisTick_.empty()) return;
    engine::shared::BlockChangeEvent ev;
    ev.tick = ticks_.load(std::memory_order_relaxed);
    ev.dirtyChunks.assign(dirtyChunksThisTick_.begin(), dirtyChunksThisTick_.end());
    if (q.push(ev)) {
        dirtyChunksThisTick_.clear();
        changeEventsSent_.fetch_add(1, std::memory_order_relaxed);
    }
    // push 失败（队列满）不清空：下 tick 重试。变更事件可延迟送达，
    // 渲染端最终一致（与位置快照"满则丢"不同：丢变更 = 渲染端漏重建）。
}
long long BusinessWorld::changeEventsSent() const {
    return changeEventsSent_.load(std::memory_order_relaxed);
}

void BusinessWorld::receiveFromPhysics(
    engine::core::SPSCQueue<engine::shared::PhysicsResult>& q) {
    // 业务 Tick 边界：收物理结果（位置快照/碰撞体数量）。
    engine::shared::PhysicsResult res;
    while (q.pop(res)) {
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

std::vector<engine::shared::Block> BusinessWorld::renderSnapshot() const {
    // 第 4 轨只读：与业务线程结构性修改并发，锁内生成一份安全快照。
    // 数据层 ② 区块化：遍历所有已加载 chunk 的非空气方块。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    std::vector<engine::shared::Block> out;
    out.reserve(chunks_.size() * 64); // 粗略预留，减少扩容
    for (const auto& [cp, data] : chunks_) {
        const int bx = cp.x << 4, by = cp.y << 4, bz = cp.z << 4; // chunk 起点世界坐标
        for (int i = 0; i < engine::shared::CHUNK_VOLUME; ++i) {
            const std::uint8_t t = data.blocks[i];
            if (t == static_cast<std::uint8_t>(engine::shared::BlockType::Air)) continue;
            out.push_back(engine::shared::Block{
                static_cast<engine::shared::BlockType>(t),
                engine::shared::BlockPos{bx + (i & 15), by + ((i >> 4) & 15), bz + (i >> 8)},
                -1}); // 主世界方块 contraptionId 恒 -1（chunk 不存该字段）
        }
    }
    return out;
}
std::size_t BusinessWorld::blockCount() const {
    // main 线程只读（日志/断言）与业务线程结构性修改可能并发——加锁读。
    // 区块化后 = chunk 非空气方块数。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    std::size_t n = 0;
    for (const auto& [cp, data] : chunks_) {
        for (const std::uint8_t t : data.blocks)
            if (t != static_cast<std::uint8_t>(engine::shared::BlockType::Air)) ++n;
    }
    return n;
}
std::size_t BusinessWorld::chunkCount() const {
    std::lock_guard<std::mutex> lock(blocksMutex_);
    return chunks_.size();
}
void BusinessWorld::forEachBlockInAABB(
    const engine::shared::BlockPos& min, const engine::shared::BlockPos& max,
    const std::function<void(const engine::shared::BlockPos&,
                             engine::shared::BlockType)>& fn) const {
    // 区域查询（跨 chunk；同步/LOD 生成/变更广播共用）：遍历区域内每个坐标，
    // chunk 定位 O(1)。原型按坐标遍历（区域通常 = chunk 级 16³=4096 次查询，可接受；
    // 渲染端想烘焙 mesh 时扩展 1 格拉邻居即可，Minetest retrieveBlocks 同思想）。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    for (int x = min.x; x <= max.x; ++x)
        for (int y = min.y; y <= max.y; ++y)
            for (int z = min.z; z <= max.z; ++z) {
                const engine::shared::BlockPos p{x, y, z};
                const auto it = chunks_.find(engine::shared::chunkOf(p));
                if (it == chunks_.end()) continue;
                const std::uint8_t t = it->second.atWorld(p);
                if (t == static_cast<std::uint8_t>(engine::shared::BlockType::Air)) continue;
                fn(p, static_cast<engine::shared::BlockType>(t));
            }
}
std::uint32_t BusinessWorld::chunkDirtyFlags(const engine::shared::ChunkPos& cp) const {
    std::lock_guard<std::mutex> lock(blocksMutex_);
    const auto it = chunks_.find(cp);
    return it == chunks_.end() ? 0u : it->second.dirty;
}
std::optional<engine::shared::Block> BusinessWorld::findBlock
(const engine::shared::BlockPos& p) const {
    // 调用方需保证无并发写（业务线程持锁调用 / main 在三轨停止后调用）。
    // 主世界 → chunk 定位 O(1)（chunk 存储只存类型，返回现场构造的 Block）。
    const auto it = chunks_.find(engine::shared::chunkOf(p));
    if (it == chunks_.end()) return std::nullopt;
    const std::uint8_t t = it->second.atWorld(p);
    if (t == static_cast<std::uint8_t>(engine::shared::BlockType::Air))
        return std::nullopt;
    return engine::shared::Block{static_cast<engine::shared::BlockType>(t), p, -1};
}
bool BusinessWorld::placeBlock(const engine::shared::BlockPos& p,
                               engine::shared::BlockType type) {
    std::lock_guard<std::mutex> lock(blocksMutex_);
    // 裁决：位置已被占用 → 拒绝放置。
    if (findBlock(p).has_value()) {
        placementsRejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // 区块化：主世界 → chunk 写入（16³ 密集数组，O(1)）。
    const auto cp = engine::shared::chunkOf(p);
    auto& c = chunks_[cp];
    c.atWorld(p) = static_cast<std::uint8_t>(type);
    c.markDirty(0x08);              // 修改跟踪：放置标记脏（持久留档）
    // 脏区块边界写入
    dirtyChunksThisTick_.insert(cp); // 变更事件流：本 tick 事件源
    const int lx = p.x & 15, ly = p.y & 15, lz = p.z & 15;
    if (lx == 0)  dirtyChunksThisTick_.insert({ cp.x - 1, cp.y, cp.z });
    if (lx == 15) dirtyChunksThisTick_.insert({ cp.x + 1, cp.y, cp.z });
    if (ly == 0)  dirtyChunksThisTick_.insert({ cp.x, cp.y - 1, cp.z });
    if (ly == 15) dirtyChunksThisTick_.insert({ cp.x, cp.y + 1, cp.z });
    if (lz == 0)  dirtyChunksThisTick_.insert({ cp.x, cp.y, cp.z - 1 });
    if (lz == 15) dirtyChunksThisTick_.insert({ cp.x, cp.y, cp.z + 1 });
    blocksPlaced_.fetch_add(1, std::memory_order_relaxed);
    // 自动排队 PlaceBlock 指令（下次 syncToPhysics 发出）。
    // 原型注意：若上一条指令因队列满未发出，本请求会覆盖待发坐标（低概率，接受）。
    wantPlace_ = true;
    placePos_ = p;
    placeType_ = static_cast<int>(type);
    return true;
}

bool BusinessWorld::removeBlock(const engine::shared::BlockPos& p) {
    std::lock_guard<std::mutex> lock(blocksMutex_);
    // 裁决：目标方块必须存在。主世界 → chunk 定位 O(1) 删除。
    auto it = chunks_.find(engine::shared::chunkOf(p));
    if (it == chunks_.end() ||
        it->second.atWorld(p) ==
            static_cast<std::uint8_t>(engine::shared::BlockType::Air)) {
        removalsRejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    it->second.atWorld(p) =
        static_cast<std::uint8_t>(engine::shared::BlockType::Air);
    it->second.markDirty(0x10);              // 修改跟踪：破坏标记脏（持久留档）
    const auto cp = it->first;
    dirtyChunksThisTick_.insert(cp);  // 变更事件流：本 tick 事件源
    //判断是否边界写入
    const int lx = p.x & 15, ly = p.y & 15, lz = p.z & 15;
    if (lx == 0)  dirtyChunksThisTick_.insert({ cp.x - 1, cp.y, cp.z });
    if (lx == 15) dirtyChunksThisTick_.insert({ cp.x + 1, cp.y, cp.z });
    if (ly == 0)  dirtyChunksThisTick_.insert({ cp.x, cp.y - 1, cp.z });
    if (ly == 15) dirtyChunksThisTick_.insert({ cp.x, cp.y + 1, cp.z });
    if (lz == 0)  dirtyChunksThisTick_.insert({ cp.x, cp.y, cp.z - 1 });
    if (lz == 15) dirtyChunksThisTick_.insert({ cp.x, cp.y, cp.z + 1 });
    blocksRemoved_.fetch_add(1, std::memory_order_relaxed);
    // 自动排队 RemoveBlock 指令（下次 syncToPhysics 发出）。
    wantRemove_ = true;
    removePos_ = p;
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
    return physicsBlockCount_.load(std::memory_order_relaxed);
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

#include "business/BusinessWorld.hpp"
#include "core/Log.hpp"

#include <cmath>
#include <sstream>

namespace engine::business {

BusinessWorld::BusinessWorld(engine::core::TimeManager& tm)
    : tm_(tm),
      startWall_(std::chrono::steady_clock::now()),
      lastLog_(std::chrono::steady_clock::now()) {
    // 原型：初始世界放一艘 3×3×3 木头飞船 + 一块飞行路径上的石头障碍物。
    // 数据层 ② 区块化：方块写入 16³ chunk（chunks_ 稀疏映射，空 chunk 不占内存）。
    // 构造时写入初始内容；此后仅业务线程读写（放置/破坏/打包取数），
    // main 只在三轨停止后读取 blockCount/findBlock，无并发写。
    auto setBlock = [&](int x, int y, int z, engine::shared::BlockType t) {
        const engine::shared::BlockPos p{x, y, z};
        auto& c = chunks_[engine::shared::chunkOf(p)]; // operator[] 缺省构造新 chunk
        c.atWorld(p) = static_cast<std::uint8_t>(t);
        c.markDirty(0x01); // 修改跟踪：初始装载标记脏（变更事件流种子，见文档 7.6）
    };
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z)
                setBlock(x, y, z, engine::shared::BlockType::Wood);
    // 障碍物放在飞船起飞路径 (50,0,0)：飞船撞上 → 碰撞事件 → 业务裁决破坏它。
    // 打包指令只打包飞船区域方块，障碍物留在静态世界（打包 = 选择子集，架构 2.7）。
    setBlock(50, 0, 0, engine::shared::BlockType::Stone);
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
    // 请求打包：此刻从主世界 chunk 快照"飞船区域"方块（|x|,|y|,|z|<=1，业务裁决
    // 选择哪些方块进刚体），排队一条 PackContraption 指令。
    // 障碍物等主世界静态方块留在静态碰撞体集合（架构 2.7 物理形态转移）。
    // 区块化后直接按坐标查询 27 格（chunk 定位 O(1)），不再线性遍历全表。
    if (wantSync_ || wantPack_ || wantUnpack_) return false;
    std::lock_guard<std::mutex> lock(blocksMutex_);
    packBlocks_.clear();
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z) {
                const engine::shared::BlockPos p{x, y, z};
                const auto it = chunks_.find(engine::shared::chunkOf(p));
                if (it == chunks_.end()) continue;
                const std::uint8_t t = it->second.atWorld(p);
                if (t == static_cast<std::uint8_t>(engine::shared::BlockType::Air)) continue;
                packBlocks_.push_back(
                    engine::shared::Block{static_cast<engine::shared::BlockType>(t), p, -1});
            }
    // 记录刚体内方块坐标：碰撞裁决保护（刚体自身方块不可被撞毁）。
    packedBlocks_.clear();
    for (const auto& b : packBlocks_) packedBlocks_.insert(b.pos);

    // 物理形态转移（架构 2.7）的业务侧同步：打包 = 方块从主世界转进刚体，
    // 业务侧 chunk 同步置空——否则打包期间渲染会看到"幽灵飞船"（主世界残留
    // + 刚体双份），且解包若不清全表会残留旧位置方块。
    for (const auto& b : packBlocks_) {
        const auto cp = engine::shared::chunkOf(b.pos);
        auto it = chunks_.find(cp);
        if (it != chunks_.end() &&
            it->second.atWorld(b.pos) !=
                static_cast<std::uint8_t>(engine::shared::BlockType::Air)) {
            it->second.atWorld(b.pos) =
                static_cast<std::uint8_t>(engine::shared::BlockType::Air);
            it->second.markDirty(0x20);        // 修改跟踪：打包移除标记脏
            dirtyChunksThisTick_.insert(cp);   // 变更事件流：本 tick 事件源
        }
    }
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
        // 区块化后从 chunks_ + bodyBlocks_ 现场生成全量列表（协议边界保持 vector<Block>）。
        cmd.type = engine::shared::PhysicsCommand::Type::SyncBlockSet;
        cmd.blocks = renderSnapshot();
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

void BusinessWorld::flushChanges(
    engine::core::SPSCQueue<engine::shared::BlockChangeEvent>& q) {
    // 业务 Tick 边界：把本 tick 变脏的 chunk 聚合为一条变更事件发出（数据层 ② 第四项）。
    // 事件源 = dirtyChunksThisTick_（写操作插入，自动去重）；发完清空。
    // 刚体域（bodyBlocks_）变更不进此流：渲染端把复合体当动态渲染单元，
    // 其状态由物理 RigidBodyPos 位置快照驱动（已有通道）。
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
            // 解包闭环（架构 2.7）：用物理回执的世界坐标方块写入主世界 chunk。
            // 业务权威的坐标与物理回写位置对齐——飞船飞了多远，方块表就记多远，
            // 不再停留在打包前的局部坐标 (-1..1)。
            // 注意：不清空 chunks_ 全表——打包时已把飞船旧位置置空（形态转移
            // 的业务侧同步），主世界其他方块（玩家放置等）必须保留；
            // 只清空刚体域 bodyBlocks_（解包 = 刚体整体拆回主世界，物理回执
            // 即为全量权威，改造方块一并失效）。
            {
                std::lock_guard<std::mutex> lock(blocksMutex_);
                bodyBlocks_.clear();
                for (const auto& b : res.blocks) {
                    // 回执方块均为世界坐标主世界方块（物理解包填充，见 PhysicsWorld）。
                    const auto cp = engine::shared::chunkOf(b.pos);
                    auto& c = chunks_[cp];
                    c.atWorld(b.pos) = static_cast<std::uint8_t>(b.type);
                    c.markDirty(0x02); // 修改跟踪：解包重建标记脏（持久留档）
                    dirtyChunksThisTick_.insert(cp); // 变更事件流：本 tick 事件源（去重）
                }
            }
            break;
        case engine::shared::PhysicsResult::Type::RigidBodyPos:
            // 刚体（飞船）位置快照：业务视角更新飞船坐标（日志/伪渲染用最新值）。
            // 同时写入插值基座双槽：保留最近两个带模拟时间戳的快照（渲染 lerp 用）。
            shipX_.store(res.rx, std::memory_order_relaxed);
            shipY_.store(res.ry, std::memory_order_relaxed);
            shipZ_.store(res.rz, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(posMutex_);
                const int other = (posNewest_ == 0) ? 1 : 0; // 写非最新槽（覆盖最旧）
                posSamples_[other] = PosSample{res.simTime, res.rx, res.ry, res.rz};
                posNewest_ = other;
                ++posCount_;
            }
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
                // 区块化后主世界破坏 O(1)：chunk 定位 → 置 Air。只破坏主世界静态方块
                // （刚体自身方块受 packedBlocks_ 保护，已在上面拦截）。
                auto it = chunks_.find(engine::shared::chunkOf(cp));
                if (it != chunks_.end() &&
                    it->second.atWorld(cp) !=
                        static_cast<std::uint8_t>(engine::shared::BlockType::Air)) {
                    it->second.atWorld(cp) =
                        static_cast<std::uint8_t>(engine::shared::BlockType::Air);
                    it->second.markDirty(0x04);        // 修改跟踪：碰撞破坏标记脏
                    dirtyChunksThisTick_.insert(it->first); // 变更事件流：本 tick 事件源
                    wantRemove_ = true;
                    removePos_ = cp; // 下发 RemoveBlock：物理删碰撞体
                    removeCid_ = -1;
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

std::vector<engine::shared::Block> BusinessWorld::renderSnapshot() const {
    // 第 4 轨只读：与业务线程结构性修改并发，锁内生成一份安全快照。
    // 数据层 ② 区块化：遍历所有已加载 chunk 的非空气方块 + 刚体域方块。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    std::vector<engine::shared::Block> out;
    out.reserve(chunks_.size() * 64 + bodyBlocks_.size()); // 粗略预留，减少扩容
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
    for (const auto& [pos, b] : bodyBlocks_) out.push_back(b);
    return out;
}
std::size_t BusinessWorld::blockCount() const {
    // main 线程伪渲染/断言与业务线程结构性修改可能并发——加锁读。
    // 区块化后 = chunk 非空气方块数 + 刚体域方块数。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    std::size_t n = 0;
    for (const auto& [cp, data] : chunks_) {
        for (const std::uint8_t t : data.blocks)
            if (t != static_cast<std::uint8_t>(engine::shared::BlockType::Air)) ++n;
    }
    return n + bodyBlocks_.size();
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
std::optional<engine::shared::Block> BusinessWorld::findBlock(
    const engine::shared::BlockPos& p, int contraptionId) const {
    // 调用方需保证无并发写（业务线程持锁调用 / main 在三轨停止后调用）。
    // 区块化后按域分流：主世界 → chunk 定位 O(1)；刚体域 → bodyBlocks_。
    if (contraptionId < 0) {
        const auto it = chunks_.find(engine::shared::chunkOf(p));
        if (it == chunks_.end()) return std::nullopt;
        const std::uint8_t t = it->second.atWorld(p);
        if (t == static_cast<std::uint8_t>(engine::shared::BlockType::Air))
            return std::nullopt;
        return engine::shared::Block{
            static_cast<engine::shared::BlockType>(t), p, -1};
    }
    const auto it = bodyBlocks_.find(p);
    return it == bodyBlocks_.end() ? std::nullopt
                                   : std::optional<engine::shared::Block>(it->second);
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
    if (findBlock(p, contraptionId).has_value()) {
        placementsRejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // 区块化：主世界 → chunk 写入（16³ 密集数组，O(1)）；刚体域 → bodyBlocks_。
    if (contraptionId < 0) {
        const auto cp = engine::shared::chunkOf(p);
        auto& c = chunks_[cp];
        c.atWorld(p) = static_cast<std::uint8_t>(type);
        c.markDirty(0x08);      // 修改跟踪：放置标记脏（持久留档）
        dirtyChunksThisTick_.insert(cp); // 变更事件流：本 tick 事件源
    } else {
        bodyBlocks_.emplace(p, engine::shared::Block{type, p, contraptionId});
    }
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
    // 裁决②：目标方块必须存在。区块化后按域分流 O(1) 删除。
    if (contraptionId < 0) {
        auto it = chunks_.find(engine::shared::chunkOf(p));
        if (it == chunks_.end() ||
            it->second.atWorld(p) ==
                static_cast<std::uint8_t>(engine::shared::BlockType::Air)) {
            removalsRejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        it->second.atWorld(p) =
            static_cast<std::uint8_t>(engine::shared::BlockType::Air);
        it->second.markDirty(0x10);        // 修改跟踪：破坏标记脏（持久留档）
        dirtyChunksThisTick_.insert(it->first); // 变更事件流：本 tick 事件源
    } else {
        const auto it = bodyBlocks_.find(p);
        if (it == bodyBlocks_.end()) {
            removalsRejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        bodyBlocks_.erase(it);
    }
    blocksRemoved_.fetch_add(1, std::memory_order_relaxed);
    // 自动排队 RemoveBlock 指令（下次 syncToPhysics 发出）。
    wantRemove_ = true;
    removePos_ = p;
    removeCid_ = contraptionId;
    return true;
}

int BusinessWorld::contraptionState() const {
    return contraptionState_.load(std::memory_order_relaxed);
}
double BusinessWorld::shipX() const { return shipX_.load(std::memory_order_relaxed); }
double BusinessWorld::shipY() const { return shipY_.load(std::memory_order_relaxed); }
double BusinessWorld::shipZ() const { return shipZ_.load(std::memory_order_relaxed); }

bool BusinessWorld::sampleShipPos(double simT, double& x, double& y, double& z) const {
    // 渲染插值基座：按查询模拟时刻在最近两个快照间线性插值。
    // 窗口外钳制：晚于最新 → 返回最新（无未来数据，等下一快照）；早于最旧 → 返回最旧。
    std::lock_guard<std::mutex> lock(posMutex_);
    if (posNewest_ < 0) return false;                 // 尚无任何快照
    const PosSample& n = posSamples_[posNewest_];    // 最新
    if (posCount_ < 2) {                              // 只有一个快照：无插值窗口
        x = n.x; y = n.y; z = n.z;
        return true;
    }
    const PosSample& o = posSamples_[1 - posNewest_]; // 次新
    if (n.simT <= o.simT) { x = n.x; y = n.y; z = n.z; return true; } // 防御：时间倒序
    if (simT >= n.simT) { x = n.x; y = n.y; z = n.z; return true; }   // 钳最新
    if (simT <= o.simT) { x = o.x; y = o.y; z = o.z; return true; }   // 钳最旧
    const double k = (simT - o.simT) / (n.simT - o.simT);             // 0~1 插值比例
    x = o.x + (n.x - o.x) * k;
    y = o.y + (n.y - o.y) * k;
    z = o.z + (n.z - o.z) * k;
    return true;
}
bool BusinessWorld::lastTwoPosSamples(PosSample& old, PosSample& latest) const {
    std::lock_guard<std::mutex> lock(posMutex_);
    if (posNewest_ < 0 || posCount_ < 2) return false;
    latest = posSamples_[posNewest_];
    old    = posSamples_[1 - posNewest_];
    return true;
}
int BusinessWorld::posSampleCount() const {
    std::lock_guard<std::mutex> lock(posMutex_);
    return posCount_;
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
long long BusinessWorld::collisions() const {
    return collisions_.load(std::memory_order_relaxed);
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
           << " 碰撞=" << collisions()
           << " 打包状态=" << contraptionState()
           << " 飞船=(" << shipX() << "," << shipY() << "," << shipZ() << ")"
           << "  生效系数=" << tm_.effectiveScale(engine::core::Domain::Business);
        engine::core::log(engine::core::LogLevel::Info, os.str());
    }
}

} // namespace engine::business

#include "worldgen/streaming/ChunkStreamer.hpp"

#include "business/World.hpp"
#include "shared/block/Block.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace engine::worldgen {

ChunkStreamer::ChunkStreamer(const ColumnGenerator& generator, int radius,
                             std::size_t requestCapacity, std::size_t resultCapacity)
    : generator_(generator),
      radius_(radius > 0 ? radius : 1),
      requests_(requestCapacity),
      results_(resultCapacity) {}

ChunkStreamer::~ChunkStreamer() {
    stop();
}

void ChunkStreamer::start() {
    if (started_.load(std::memory_order_relaxed)) return;
    started_.store(true, std::memory_order_relaxed);
    loop_.start([this] { workerLoop(); });
}

void ChunkStreamer::stop() {
    if (!started_.load(std::memory_order_relaxed)) return;
    started_.store(false, std::memory_order_relaxed);
    loop_.stop();   // running_=false + join：生成线程在下一个检查点退出
}

bool ChunkStreamer::running() const {
    return started_.load(std::memory_order_relaxed);
}

void ChunkStreamer::workerLoop() {
    // 生成线程：只做纯计算，不持有、不访问 World（8.4 条准则的硬边界）
    while (!loop_.shouldStop()) {
        GenRequest req;
        if (!requests_.pop(req)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); // 无请求：让出 CPU
            continue;
        }

        auto blocks = std::make_shared<ColumnBlocks>();
        generator_.generateColumn(req.pos, *blocks);
        generated_.fetch_add(1, std::memory_order_relaxed);

        // 结果队列满 = 业务线程还没消费完：等它（有界等待，不丢结果）
        const GenResult res{req.pos, std::move(blocks)};
        while (!results_.push(res)) {
            if (loop_.shouldStop()) return;   // 收工：不再阻塞退出流程
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void ChunkStreamer::tickUpdate(double playerX, double playerZ) {
    // 世界坐标 → 列坐标：先 floor 再 >>4（对负数也正确：-0.5 属列 -1）
    const int cx = static_cast<int>(std::floor(playerX)) >> 4;
    const int cz = static_cast<int>(std::floor(playerZ)) >> 4;

    // ① 淘汰：走出"加载半径 + 1"的列进入待卸载集合。
    // 比加载半径多留一圈 = 滞回，避免玩家站在窗口边界时同一列反复卸载/重建。
    // 这里只摘 known_、收集坐标；真正删 chunk 在 drainResults（那里才持有 World）。
    const int evictR = radius_ + kEvictMargin;
    for (auto it = known_.begin(); it != known_.end(); ) {
        const int dx = it->x - cx;
        const int dz = it->z - cz;
        if (std::abs(dx) > evictR || std::abs(dz) > evictR) {
            pendingEvict_.insert(*it);
            it = known_.erase(it);   // 摘掉记录 → 走回来会重新请求
        } else {
            ++it;
        }
    }

    // ② 收集窗口内尚未请求的列。窗口固定 (2r+1)×(2r+1) 格，每 tick 全扫一遍；
    // 已请求的列被 known_ 挡住 → 稳定后每 tick 只做 81 次哈希查询，开销可忽略。
    std::vector<ColPos> candidates;
    const int span = 2 * radius_ + 1;
    candidates.reserve(static_cast<std::size_t>(span * span));
    for (int dz = -radius_; dz <= radius_; ++dz) {
        for (int dx = -radius_; dx <= radius_; ++dx) {
            const ColPos p{cx + dx, cz + dz};
            if (known_.find(p) != known_.end()) continue;
            candidates.push_back(p);
        }
    }
    if (candidates.empty()) return;

    // 近的优先：窗口首次铺开时先到货的是玩家脚下那几列（视觉上"脚下先长出地面"）
    std::sort(candidates.begin(), candidates.end(),
              [cx, cz](const ColPos& a, const ColPos& b) {
                  const int ax = a.x - cx, az = a.z - cz;
                  const int bx = b.x - cx, bz = b.z - cz;
                  return (ax * ax + az * az) < (bx * bx + bz * bz);
              });

    for (const ColPos& p : candidates) {
        if (!requests_.push(GenRequest{p})) break;   // 队列满：剩下的下 tick 再推
        known_.insert(p);                            // 只记成功的；失败的不能记，否则永不重试
        requested_.fetch_add(1, std::memory_order_relaxed);
    }
}

ChunkStreamer::DrainOutcome ChunkStreamer::drainResults(engine::business::World& world,
                                                        std::size_t maxApply) {
    DrainOutcome out;

    // ① 先结算淘汰：一列 = 4 个竖直堆叠的 16³ chunk。
    // World::unloadChunks 会标脏"自身 + 还在的邻居"，渲染端重建时取不到方块就删 mesh，
    // 不需要给渲染端新增"卸载"接口。
    if (!pendingEvict_.empty()) {
        std::unordered_set<engine::shared::ChunkPos> dead;
        dead.reserve(pendingEvict_.size() * static_cast<std::size_t>(kGenChunkLevels));
        for (const ColPos& col : pendingEvict_) {
            for (int level = 0; level < kGenChunkLevels; ++level) {
                dead.insert(engine::shared::ChunkPos{
                    col.x, (kGenMinY / engine::shared::CHUNK_SIZE) + level, col.z});
            }
        }
        out.evictedChunks = world.unloadChunks(dead);
        pendingEvict_.clear();
    }

    // ② 应用生成结果：已被淘汰的列直接丢弃——否则会把刚删掉的 chunk 又写回来
    //    （队列里可能还压着淘汰前排队/在算的请求，这是允许的浪费，但结果不能落地）。
    std::size_t applied = 0;
    GenResult res;
    while (applied < maxApply && results_.pop(res)) {
        if (known_.find(res.pos) == known_.end()) continue;   // 在途结果属于已淘汰的列
        // 一列 = 4 个竖直堆叠的 16³ chunk：逐层写入（每个 chunk 一次锁 + 标脏自身与邻居）
        for (int level = 0; level < kGenChunkLevels; ++level) {
            const engine::shared::ChunkPos cp{
                res.pos.x,
                (kGenMinY / engine::shared::CHUNK_SIZE) + level,
                res.pos.z};
            world.applyGeneratedChunk(cp, res.blocks->chunks[level]);
        }
        applied_.fetch_add(1, std::memory_order_relaxed);
        // 聚合统计（本线程独占，无并发）
        appliedBlocks_ += res.blocks->blockCount;
        minSurfaceY_ = std::min(minSurfaceY_, res.blocks->minSurfaceY);
        maxSurfaceY_ = std::max(maxSurfaceY_, res.blocks->maxSurfaceY);
        ++applied;
    }
    out.appliedColumns = applied;
    return out;
}

GenStats ChunkStreamer::stats() const {
    return GenStats{
        requested_.load(std::memory_order_relaxed),
        generated_.load(std::memory_order_relaxed),
        applied_.load(std::memory_order_relaxed),
        appliedBlocks_,
        minSurfaceY_,
        maxSurfaceY_};
}

} // namespace engine::worldgen

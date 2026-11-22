#include "worldgen/streaming/ChunkStreamer.hpp"

#include "shared/chunk/ChunkPos.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace engine::worldgen {

namespace {

// ③ 近的优先：窗口首次铺开时先到货的是玩家脚下那几列（视觉上"脚下先长出地面"）
void sortByDistanceTo(std::vector<ColPos>& candidates, int cx, int cz) {
    std::sort(candidates.begin(), candidates.end(),
              [cx, cz](const ColPos& a, const ColPos& b) {
                  const int ax = a.x - cx, az = a.z - cz;
                  const int bx = b.x - cx, bz = b.z - cz;
                  return (ax * ax + az * az) < (bx * bx + bz * bz);
              });
}

} // namespace

ChunkStreamer::ChunkStreamer(const ColumnGenerator& generator, int radius,
                             std::size_t requestCapacity, std::size_t resultCapacity)
    : generator_(generator),
      radius_(radius > 0 ? radius : 1),
      businessToGenRequests_(requestCapacity),
      genToBusinessResults_(resultCapacity) {}

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
        if (!businessToGenRequests_.pop(req)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); // 无请求：让出 CPU
            continue;
        }

        auto blocks = std::make_shared<ColumnBlocks>();
        generator_.generateColumn(req.pos, *blocks);
        generatedColumnCount_.fetch_add(1, std::memory_order_relaxed);

        // 结果队列满 = 业务线程还没消费完：等它（有界等待，不丢结果）
        const GenResult res{req.pos, std::move(blocks)};
        while (!genToBusinessResults_.push(res)) {
            if (loop_.shouldStop()) return;   // 收工：不再阻塞退出流程
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void ChunkStreamer::tick(double playerX, double playerZ) {
    // 世界坐标 → 列坐标：先 floor 再 >>4（对负数也正确：-0.5 属列 -1）
    const int cx = static_cast<int>(std::floor(playerX)) >> 4;
    const int cz = static_cast<int>(std::floor(playerZ)) >> 4 ;

    // ① 淘汰：走出"加载半径 + 1"的列进入待卸载集合。
    // 比加载半径多留一圈 = 滞回，避免玩家站在窗口边界时同一列反复卸载/重建。
    // 这里只摘 known_、收集坐标；真正删 chunk 在 receiveResults（那里才持有 World）。
    collectEvictions(cx, cz);

    // ② 收集窗口内尚未请求的列。窗口固定 (2r+1)×(2r+1) 格，每 tick 全扫一遍；
    // 已请求的列被 known_ 挡住 → 稳定后每 tick 只做 81 次哈希查询，开销可忽略。
    std::vector<ColPos> candidates = collectWindowCandidates(cx, cz);
    if (candidates.empty()) return;

    // ③ 近的优先
    sortByDistanceTo(candidates, cx, cz);

    // ④ 入队（队列满：剩下的下 tick 再推）
    enqueueRequests(candidates);
}

void ChunkStreamer::collectEvictions(int cx, int cz) {
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
}

std::vector<ColPos> ChunkStreamer::collectWindowCandidates(int cx, int cz) const {
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
    return candidates;
}

void ChunkStreamer::enqueueRequests(const std::vector<ColPos>& candidates) {
    for (const ColPos& p : candidates) {
        if (!businessToGenRequests_.push(GenRequest{p})) break;   // 队列满：剩下的下 tick 再推
        known_.insert(p);                            // 只记成功的；失败的不能记，否则永不重试
        requestedColumnCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

ChunkStreamer::StreamBatch ChunkStreamer::takeBatch(std::size_t maxColumns) {
    StreamBatch batch;

    // ① 淘汰列 → 待卸载 chunk 列表（一列 = kGenChunkLevels 个竖直 chunk）。
    //    只产数据、不删：World::unloadChunks 由调用方（业务线程）执行。
    //    World::unloadChunks 会标脏"自身 + 还在的邻居"，渲染端重建时取不到方块就删 mesh，
    //    不需要给渲染端新增"卸载"接口。
    if (!pendingEvict_.empty()) {
        batch.deadChunks.reserve(pendingEvict_.size() *
                                 static_cast<std::size_t>(kGenChunkLevels));
        for (const ColPos& col : pendingEvict_) {
            for (int level = 0; level < kGenChunkLevels; ++level) {
                batch.deadChunks.insert(engine::shared::ChunkPos{
                    col.x, (kGenMinY / engine::shared::CHUNK_SIZE) + level, col.z});
            }
        }
        pendingEvict_.clear();
    }

    // ② 取生成结果：已被淘汰的列直接丢弃 —— 否则会把刚删掉的 chunk 又写回来
    //    （队列里可能还压着淘汰前排队/在算的请求，这是允许的浪费，但结果不能落地）。
    // B127：加"最多检查 4×maxColumns 个"的上限 —— 否则队列里积压大量**已淘汰列**的
    //    结果时，一趟 while 会连着 pop 几百次（每次都要查 known_），表现为单 tick 尖峰
    //    （瞬移/快速移动后掉帧）。超出的留到下 tick 继续。
    std::size_t taken = 0;
    std::size_t inspected = 0;
    const std::size_t maxInspect = maxColumns * 4U;
    GenResult res;
    while (taken < maxColumns && inspected < maxInspect && genToBusinessResults_.pop(res)) {
        ++inspected;
        if (known_.find(res.pos) == known_.end()) continue;   // 在途结果属于已淘汰的列
        appliedColumnCount_.fetch_add(1, std::memory_order_relaxed);
        // 聚合统计（本线程独占，无并发）—— 必须在 move 之前读
        appliedBlocks_ += res.blocks->blockCount;
        minSurfaceY_ = std::min(minSurfaceY_, res.blocks->minSurfaceY);
        maxSurfaceY_ = std::max(maxSurfaceY_, res.blocks->maxSurfaceY);
        batch.columns.push_back(StreamBatch::Column{res.pos, std::move(res.blocks)});
        ++taken;
    }
    return batch;
}

GenStats ChunkStreamer::stats() const {
    return GenStats{
        requestedColumnCount_.load(std::memory_order_relaxed),
        generatedColumnCount_.load(std::memory_order_relaxed),
        appliedColumnCount_.load(std::memory_order_relaxed),
        appliedBlocks_,
        minSurfaceY_,
        maxSurfaceY_};
}

} // namespace engine::worldgen

#pragma once
#include "core/SPSCQueue.hpp"
#include "core/TimeManager.hpp"
#include "shared/Block.hpp"
#include "shared/Protocol.hpp"

#include <atomic>
#include <chrono>
#include <functional> // std::function（forEachBlockInAABB 回调）
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::business {

// 业务世界（20TPS，dt=0.05s）：世界状态的业务权威。
// 职责：维护 sim_time/tick、方块表（存在性/类型裁决）、打包/解包裁决、
//       放置/破坏裁决（2.7：存在性/类型归业务裁决，物理只同步碰撞体）。
// 注意：演示事件（tick 触发的放置/打包/改造）已剥离到 demo::DemoDriver，
//       本类只提供"命令式"接口——请求 → 裁决 → 排队指令 → syncToPhysics 发出。
class BusinessWorld {
public:
    explicit BusinessWorld(engine::core::TimeManager& tm);

    // 推进一个业务 tick：simTime += 0.05 * time_scale
    void advanceTick(double scale);

    // 业务 Tick 边界：下发刚体指令到物理世界。
    void syncToPhysics(engine::core::SPSCQueue<engine::shared::PhysicsCommand>& q);

    // 业务 Tick 边界：收取物理结果（刚体位置回传）。
    void receiveFromPhysics(engine::core::SPSCQueue<engine::shared::PhysicsResult>& q);

    // 业务 Tick 边界：变更事件流（数据层 ② 第四项，渲染快照正式化）。
    // 把本 tick 变脏的 chunk 聚合为一条变更事件发给渲染端（第 4 轨只读）：
    // 渲染端消费 → 标记 chunk 脏 → 只重建变过的 chunk mesh（不全量重画）。
    void flushChanges(engine::core::SPSCQueue<engine::shared::BlockChangeEvent>& q);
    long long changeEventsSent() const; // 已发变更事件数（断言/诊断）

    // ---- 命令式接口（演示/玩家操作/脚本事件源共用）----
    // 请求全量同步：初始世界方块 → 物理碰撞体集合（排队一条 SyncBlockSet）。
    bool syncAll();
    // 请求打包：当前飞船区域方块 → 刚体（裁决选择哪些方块，排队 PackContraption）。
    // 注意：调用前飞船区域方块必须已在 blocks_（打包列表在此刻快照）。
    bool packContraption();
    // 请求解包：当前刚体 → 静态方块（排队 UnpackContraption，需先打包）。
    bool unpackContraption();
    // 当前刚体 ID（打包回执后有效；≥0 表示飞船处于打包状态）。
    int  packedId() const;

    double     simTime() const;
    long long  tickCount() const;
    long long  commandsSent() const;     // 已下发指令数
    long long  resultsReceived() const;  // 已收到物理结果数
    int        blockX() const;           // 业务视角：物理回传的方块最新坐标
    int        blockY() const;
    int        blockZ() const;

    // 世界方块查询（数据层 ② 区块化）：
    // 主世界静态方块 → 16³ chunk（chunks_，世界坐标，定位 O(1)）；
    // 刚体内方块（contraptionId≥0，飞船改造）→ bodyBlocks_（局部坐标，独立容器）。
    std::size_t blockCount() const;
    // 渲染快照：锁内拷贝全量方块列表（第 4 轨只读，main 线程运行中安全调用）。
    std::vector<engine::shared::Block> renderSnapshot() const;
    // 按坐标查方块（返回空 = 不存在）。区块化后按域分流：
    // contraptionId=-1 → chunk 定位 O(1)；≥0 → bodyBlocks_ 查找。
    // 值返回（optional）：主世界方块由 chunk 现场构造（存储只存类型），
    // 不返回内部指针（避免悬垂/并发写风险）。
    std::optional<engine::shared::Block> findBlock(const engine::shared::BlockPos& p,
                                                   int contraptionId = -1) const;

    // 已加载 chunk 数（空 chunk 不占内存；断言/诊断用）。
    std::size_t chunkCount() const;
    // 区域查询（跨 chunk；同步 / LOD 生成 / 变更广播共用）：遍历 [min, max]
    // 内所有非空气方块，对每个调用 fn。原型直接按坐标遍历（区域内坐标定位 O(1)）。
    void forEachBlockInAABB(const engine::shared::BlockPos& min,
                            const engine::shared::BlockPos& max,
                            const std::function<void(const engine::shared::BlockPos&,
                                                     engine::shared::BlockType)>& fn) const;
    // 某 chunk 的变更原因位掩码（修改跟踪诊断/断言用；未加载返回 0）。
    std::uint32_t chunkDirtyFlags(const engine::shared::ChunkPos& cp) const;

    // Contraption 打包状态（0=未打包 1=已打包 2=已解包，main 演示读取）。
    int contraptionState() const;
    // 业务视角：物理回传的刚体（飞船）最新世界位置。
    double shipX() const;
    double shipY() const;
    double shipZ() const;

    // ---- 位置插值基座（数据层 ② 位置带时间戳，渲染插值前提）----
    // 物理 RigidBodyPos 回执自带 simTime 戳；业务侧保留最近两个带戳快照，
    // 渲染端按查询模拟时刻在两快照间线性插值（lerp）。渲染帧率不受限时
    // 帧率越高插值点越密，插值机制与帧率无关。
    struct PosSample {
        double simT = 0.0;            // 物理模拟时刻（快照时刻）
        double x = 0.0, y = 0.0, z = 0.0;
    };
    // 按模拟时刻查询刚体位置：落在两快照之间 → 线性插值；
    // 晚于最新快照 → 钳到最新（无未来数据，等下一快照）；早于最旧 → 钳到最旧。
    bool sampleShipPos(double simT, double& x, double& y, double& z) const;
    // 最近两个快照（断言/调试用）：old=次新，latest=最新。返回 false=不足两个快照。
    bool lastTwoPosSamples(PosSample& old, PosSample& latest) const;
    // 诊断：已接收位置快照数（插值窗口诊断/调试用）。
    int posSampleCount() const;

    // ---- 方块放置/破坏（业务裁决，架构 2.7 + 飞船改造）----
    // 裁决规则：①目标域是刚体时飞船必须处于打包状态（刚体存在性）；
    //           ②同域位置已占用拒绝放置；③破坏不存在的方块拒绝。
    // contraptionId=-1 → 主世界静态方块；≥0 → 刚体内方块（飞船改造，
    // 物理侧增量重算碰撞箱）。裁决通过后修改 blocks_（业务权威）
    // 并自动排队 PlaceBlock/RemoveBlock 指令（下次 syncToPhysics 发出）。
    bool placeBlock(const engine::shared::BlockPos& p, engine::shared::BlockType type,
                    int contraptionId = -1);
    bool removeBlock(const engine::shared::BlockPos& p, int contraptionId = -1);

    long long blocksPlaced() const;        // 成功放置次数
    long long blocksRemoved() const;       // 成功破坏次数
    long long placementsRejected() const;  // 放置被拒次数
    long long removalsRejected() const;    // 破坏被拒次数
    long long physicsBlockCount() const;   // 物理侧静态碰撞体数量快照（回传）
    long long collisions() const;          // 收到碰撞事件次数（飞船撞上静态方块）

private:
    void maybeLog();

    engine::core::TimeManager& tm_;
    std::atomic<double>        simTime_{ 0.0 };
    std::atomic<long long>     ticks_{0};
    std::atomic<long long>     commandsSent_{0};
    std::atomic<long long>     resultsReceived_{0};
    std::atomic<int>           blockX_{0};
    std::atomic<int>           blockY_{0};
    std::atomic<int>           blockZ_{0};
    std::atomic<int>           contraptionState_{0};
    std::atomic<double>        shipX_{0.0};  // 刚体（飞船）位置快照（业务视角）
    std::atomic<double>        shipY_{0.0};
    std::atomic<double>        shipZ_{0.0};
    // 世界方块存储（数据层 ② 区块化）：主世界与刚体域分容器。
    // 主世界静态方块（contraptionId=-1，世界坐标）→ chunks_（16³ 密集 uint8，
    // 空 chunk 不占内存；定位 O(1)）。刚体内方块（contraptionId≥0，局部坐标，
    // 飞船改造）→ bodyBlocks_（独立容器，不进 chunk——两个坐标空间独立，
    // 硬塞进 chunk 会与主世界同坐标方块冲突）。
    // 两容器仅业务线程结构性修改；main 线程伪渲染/断言在锁内读取——blocksMutex_ 保护。
    std::unordered_map<engine::shared::ChunkPos, engine::shared::ChunkData> chunks_;
    std::unordered_map<engine::shared::BlockPos, engine::shared::Block>    bodyBlocks_;
    mutable std::mutex blocksMutex_; // const 读函数也要能加锁（blockCount/renderSnapshot）

    // 变更事件流（数据层 ② 第四项）的事件源：本 tick 变脏的 chunk 集合。
    // 写操作（放置/破坏/碰撞破坏/解包重建）除 markDirty（持久位掩码，诊断留档）
    // 外还插入本集合；flushChanges 在 tick 边界采集打包为 BlockChangeEvent 后清空。
    std::unordered_set<engine::shared::ChunkPos> dirtyChunksThisTick_;
    std::atomic<long long> changeEventsSent_{0}; // 已发变更事件数

    // 打包状态机（纯业务线程私有，无需原子）：
    bool wantSync_ = false;      // 待发全量同步指令（syncAll）
    bool wantPack_ = false;      // 待发打包命令（packContraption）
    bool wantUnpack_ = false;    // 待发解包命令（unpackContraption）
    int  packedId_ = 0;          // 物理回执的刚体 ID
    // 打包方块快照：packContraption 时从 blocks_ 选出飞船区域方块，同步时下发。
    std::vector<engine::shared::Block> packBlocks_;
    // 刚体内方块坐标集合：打包时记录，仅用于碰撞裁决（刚体自身方块不可被撞毁）；
    // 放置/破坏不再用其冻结（飞船改造 = 刚体内方块增删，架构 3.4 演进）。
    std::unordered_set<engine::shared::BlockPos> packedBlocks_;

    // 放置/破坏待发指令（placeBlock/removeBlock 裁决通过后自动排队）：
    bool wantPlace_ = false;                  // 待发放置指令
    engine::shared::BlockPos placePos_;       // 放置坐标
    int  placeType_ = 0;                      // 放置类型（BlockType 底层值）
    int  placeCid_ = -1;                      // 放置目标域（-1=主世界，≥0=刚体）
    bool wantRemove_ = false;                 // 待发破坏指令
    engine::shared::BlockPos removePos_;      // 破坏坐标
    int  removeCid_ = -1;                     // 破坏目标域（-1=主世界，≥0=刚体）

    std::atomic<long long> blocksPlaced_{0};       // 成功放置（裁决通过）次数
    std::atomic<long long> blocksRemoved_{0};      // 成功破坏次数
    std::atomic<long long> placementsRejected_{0}; // 放置被拒次数
    std::atomic<long long> removalsRejected_{0};   // 破坏被拒次数
    std::atomic<long long> physicsBlockCount_{0};  // 物理碰撞体数量快照
    std::atomic<long long> collisions_{0};         // 收到碰撞事件次数
    std::chrono::steady_clock::time_point lastLog_;

    // ---- 位置插值基座：双槽快照（业务线程写 / main 渲染读）----
    // 环形双槽恒存"最近两个"带模拟时间戳的位置快照：新快照覆盖次新槽，
    // 随后最新槽翻转。posMutex_ 保护（更新 25Hz、读取 60Hz+，锁竞争可忽略，
    // 性能敏感后再升级无锁 seqlock）。
    mutable std::mutex posMutex_;
    PosSample posSamples_[2];  // 槽 0/1 轮换；posNewest_ 指向最新槽
    int       posNewest_ = -1; // -1=尚无快照
    int       posCount_  = 0;  // 累计快照数（<2 时无插值窗口）
};

} // namespace engine::business

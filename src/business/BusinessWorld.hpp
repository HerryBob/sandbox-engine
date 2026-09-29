#pragma once
#include "core/SPSCQueue.hpp"
#include "core/TimeManager.hpp"
#include "shared/Block.hpp"
#include "shared/Protocol.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
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

    // 世界方块列表（原型：初始一艘 3×3×3 木头飞船，后续打包/解包的数据源）。
    const std::vector<engine::shared::Block>& blocks() const;
    std::size_t blockCount() const;
    // 渲染快照：锁内拷贝方块列表（第 4 轨只读，main 线程运行中安全调用）。
    std::vector<engine::shared::Block> renderSnapshot() const;
    // 按坐标查方块（找不到返回 nullptr；原型线性查找，将来换坐标映射）。
    // contraptionId：-1=主世界方块，≥0=刚体内方块（两个空间坐标独立，互不冲突）。
    const engine::shared::Block* findBlock(const engine::shared::BlockPos& p,
                                           int contraptionId = -1) const;

    // Contraption 打包状态（0=未打包 1=已打包 2=已解包，main 演示读取）。
    int contraptionState() const;
    // 业务视角：物理回传的刚体（飞船）最新世界位置。
    double shipX() const;
    double shipY() const;
    double shipZ() const;

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
    // 世界方块列表：构造时写入初始内容；此后仅业务线程结构性修改（放置/破坏/重建），
    // main 线程伪渲染也会读取（架构 4.x 第 4 轨只读）——用 blocksMutex_ 保护。
    std::vector<engine::shared::Block> blocks_;
    mutable std::mutex blocksMutex_; // const 读函数也要能加锁（blockCount/renderSnapshot）

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
};

} // namespace engine::business

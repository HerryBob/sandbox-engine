#pragma once
#include "core/SPSCQueue.hpp"
#include "core/TimeManager.hpp"
#include "shared/protocol/Protocol.hpp"
#include "shared/state/PlayerBodyState.hpp"
#include "shared/state/PlayerControlState.hpp"

#include <atomic>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace engine::physics {

// 物理体运动类型（对应 Jolt 的 RigidBodyType）。
// Static 永不移动（地形，当前不进 bodies_）；Dynamic 按速度积分并做碰撞回退（方块组）；
// Kinematic 速度由外部设定、受重力、位置由碰撞修正（生物/玩家）。
enum class MotionType { Static, Dynamic, Kinematic };

// 原型最小刚体（Contraption 打包产物）。
// 位置用 double（精度设计见架构文档 2.8：复合体=基准+局部坐标）。
struct RigidBody {
    int    id = 0;              // 刚体 ID（物理分配）
    MotionType motion = MotionType::Dynamic; // 运动类型：方块组=Dynamic，生物=Kinematic
    double x = 0.0, y = 0.0, z = 0.0; // 世界位置（基准；生物为脚底中心）
    double vx = 0.0, vy = 0.0, vz = 0.0; // 速度（2.3 积分用）
    // 姿态（四元数）。当前只在打包时设一个固定值，用于验证"旋转数据通路"；
    // Jolt 接入后由刚体的真实角速度积分得到（届时删掉打包处那行临时赋值）。
    float  qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
    int    blockCount = 0;      // 打包的方块数
    // 内部方块（局部坐标 + 类型）：世界坐标 = 基准 + 局部坐标（架构 2.8）。
    // 解包时按基准偏移回写静态碰撞体集合。
    std::vector<engine::shared::Block> blocks;
    // 碰撞体半宽 / 中心偏移（相对基准）。
    // 求解器删除后这两组字段不再是"手工 AABB"：
    //   - 生物：作为 CapsuleShape 的尺寸（装配期由 addPlayerBody 传入）
    //   - 刚体：Jolt 的 compound shape 自己算，这两组字段届时可删
    double halfExtentX = 0.0, halfExtentY = 0.0, halfExtentZ = 0.0;
    double centerOffsetX = 0.0, centerOffsetY = 0.0, centerOffsetZ = 0.0;
    // 踩地标志（求解器删除后恒为 false；Jolt 接入后由 CharacterVirtual::GetGroundState 取代）。
    bool   onGround = false;
};

// 物理世界（25TPS，dt=0.04s）。
// 用累积待处理模拟时间(accumulator)驱动，不硬绑定业务 tick 次数。
//
// ⚠️ 当前状态（2026-11-19）：求解器已删除，只保留「时间推进 + 指令消费 + 回执组装」。
// 积分 / 碰撞检测 / 碰撞响应 / 运动学推进 / 接触集合 全部移除，等待 Jolt 接入
// （见 docs/DOCS自制版/未来方向-物理内核替换为Jolt.md）。
// 因此：玩家不会移动、刚体不会运动、没有碰撞 —— 这是有意的过渡状态，不是 bug。
class PhysicsWorld {
public:
    explicit PhysicsWorld(engine::core::TimeManager& tm);

    // 每墙钟帧调用一次：accumulator += wall_dt * time_scale，
    // 然后 while(accumulator>=0.04) 执行物理 tick（处理业务指令）。
    void tick(double scale, double wallDt,
                 engine::core::SPSCQueue<engine::shared::PhysicsCommand>& fromBusiness);

    // 物理 → 业务：先发打包回执（逐条，不合并），再发位置快照（合并语义）。
    void pushResultsToBusiness(engine::core::SPSCQueue<engine::shared::PhysicsResult>& q);

    double     simTime() const;
    long long  tickCount() const;
    long long  processedCommandCount() const;  // 累计处理的业务指令数
    int        blockX() const;             // 演示用"方块"的最新坐标（指令驱动）
    int        blockY() const;
    int        blockZ() const;
    long long  bodyCount() const;          // 当前刚体数（打包/解包后变化）
    long long  colliderCount() const;      // 当前静态碰撞体数量（放置/破坏/打包转移后变化）
    long long  collisionCount() const;         // 累计碰撞次数（求解器删除后恒为 0，待 ContactListener 接上）

    // 注意：bodyBlockCount / bodyHalfExtentX / bodyCenterOffsetX 三个验证 getter 已删除
    // （数据源 recomputeAABB 已删；Jolt 接入后可改用 compound shape 的 GetLocalBounds）。

    // 创建一个生物体：运动学、单 AABB、位置锚在脚底中心。
    // 碰撞箱半宽与跳跃初速由调用方按实体类型定义传入（引擎不再写死某个生物的数值）。
    // 只能在物理线程启动前调用（装配期单线程），运行期不增删生物。返回分配到的刚体 id。
    int addPlayerBody(double x, double y, double z,
                      double halfExtentX, double halfExtentY, double halfExtentZ,
                      double jumpSpeed);

    // 绑定玩家状态输出槽（相机跟随用）：物理线程每 tick 写玩家位置，输入线程读。
    // 必须在物理线程启动前调用。
    void attachPlayerState(engine::shared::PlayerBodyState* out);

    // 绑定玩家操控输入槽：物理线程每 tick 读它驱动玩家水平速度与跳跃。
    // 必须在物理线程启动前调用。
    void attachPlayerControl(engine::shared::PlayerControlState* in);

private:
    void maybeLog();

    // ---- tick 的三段（规则1 拆分）----
    // 消费业务指令队列：放置/破坏/全量同步 → 静态碰撞体集合；
    // Pack/UnpackContraption → 刚体建/拆 + 碰撞体形态转移。
    void applyCommands(engine::core::SPSCQueue<engine::shared::PhysicsCommand>& fromBusiness);
    void onSyncBlockSet(const engine::shared::PhysicsCommand& cmd);
    void onPlaceBlock(const engine::shared::PhysicsCommand& cmd);
    void onRemoveBlock(const engine::shared::PhysicsCommand& cmd);
    void onPackContraption(const engine::shared::PhysicsCommand& cmd);
    void onUnpackContraption(const engine::shared::PhysicsCommand& cmd);
    // 刚体插值快照：为玩家体成组写 PlayerBodyState（seqlock 包住 prev/curr/时刻）
    void snapshotPlayerBody(double wallDt);
    // 注意：原 recomputeAABB / moveKinematicAxis 已删除（求解器迁 Jolt 时整段替换）。
    // 二者的职责分别由 compound shape 的 GetLocalBounds() 和 CharacterVirtual 承担。

    // tick 银行上限：单帧最多补跑 5 个物理 tick（=200ms 模拟时间）。
    // 超过说明本帧严重落后（卡顿/掉帧），剩余累积时间直接丢弃，
    // 防止"越落后越要补、越补越落后"的螺旋死亡（spiral of death）。
    static constexpr int kMaxStepsPerFrame_ = 5;

    // 跳跃初速度（格/秒）：跳高 = v²/(2g)。默认 4.9 ⇒ 约 1.23 格，略高于 1 格台阶。
    // 装配期由 addPlayerBody 按实体类型定义覆盖；只在 onGround 时生效，空中按跳跃无效。
    double                     jumpSpeed_{4.9};

    engine::core::TimeManager& tm_;
    double                     accumulator_{0.0};
    std::atomic<double>        simTime_{0.0};
    std::atomic<long long>     tickCount_{0};
    std::atomic<long long>     processedCommandCount_{0};
    std::atomic<int>           blockX_{0};
    std::atomic<int>           blockY_{0};
    std::atomic<int>           blockZ_{0};
    long long                  lastSentProcessed_{0}; // 纯物理线程：上次回传时的处理数
    std::chrono::steady_clock::time_point startWall_; // 墙钟起点（tps 墙钟口径用）
    std::chrono::steady_clock::time_point lastLog_;
    // 上次写入 PlayerBodyState 的墙钟时刻（纳秒）。用于计算 tickDurationNs：
    // 两次写入的墙钟差 = 本次物理 tick 的实际墙钟间隔，自适应时间缩放和调度抖动。
    long long                  lastPlayerStateWriteNs_ = 0;
    // 刚体表（纯物理线程独占读写；bodyCount_ 是它的原子投影供 main 读）。
    std::vector<RigidBody>     bodies_;
    long long                  nextBodyId_{1};    // 刚体 ID 分配器
    int                        playerBodyId_{-1}; // 玩家体 id（-1 = 未创建）
    engine::shared::PlayerBodyState* playerState_{nullptr}; // 玩家位置输出槽（装配期注入）
    engine::shared::PlayerControlState* playerControl_{nullptr}; // 玩家操控输入槽（装配期注入）
    std::atomic<long long>     bodyCount_{0};
    std::vector<engine::shared::PhysicsResult> pendingReceipts_; // 待发回执（纯物理线程）
    std::vector<engine::shared::PhysicsResult> pendingCollisions_; // 待发碰撞事件（纯物理线程）
    std::atomic<long long> collisionCount_{0};  // 累计碰撞次数（main 读）
    double lastPosSentSimT_ = -1.0; // 纯物理线程：上次已发位置快照的模拟时刻。
                                    // 物理循环每墙钟帧调 pushResultsToBusiness，但模拟 tick 只
                                    // 25Hz——同一模拟时刻的位置被重复发 ~15 次（旧行为）。
                                    // 位置快照语义：只发"新模拟时刻"的位置（数据层②修正）。

    // 静态方块碰撞体集合（纯物理线程独占）。
    // 求解器删除后它只是"静态世界的影子"（没有碰撞消费方），保留是为了让
    // SyncBlockSet / Place / Remove 的指令语义完整；Jolt 接入后由"每 chunk 一个
    // StaticCompoundShape"取代。
    std::unordered_map<engine::shared::BlockPos, engine::shared::BlockType>  colliders_;
};

} // namespace engine::physics

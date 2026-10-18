# Minetest（Luanti）区块存储与客户端 Mesh/LOD 设计分析

> 学习参考・服务端权威 + 渲染分离的工业级参照
> 来源：GitHub 
>
> `minetest/minetest`
>
>  master 分支关键源码文件（抓取于 2026-09-30）
> 许可证：LGPL-2.1-or-later（宽松，可读可借鉴；沿用本仓库 "只学不抄" 原则）
> 分析对象文件：
>
> `src/mapblock.h`
>
> （全文）、
>
> `src/map.h`
>
> （片段）、
>
> `src/client/mesh_generator_thread.cpp`
>
> （片段）、
>
> `src/client/content_mapblock.cpp`
>
> （片段）



***

## 0. 一句话总览

Minetest 是开源版 "MC 类" 体素引擎（C++，Lua 模组），**它的区块存储（16³ MapBlock）与客户端 mesh 生成管线，几乎就是我们正在设计的 "区块化存储 + LOD 适配" 的工业级参考实现**。它没有真正的 LOD，但它的设计证明了一个核心结论：**服务端区块只管数据组织，渲染 /mesh 生成是客户端自己的事（独立线程、独立单元、独立缓存）**—— 与我们的 "渲染 = 第 4 轨只读观众" 完全同构。



***

## 1. 项目背景



* **定位**：自由开源的体素沙盒引擎 + 游戏（服务器 / 客户端分离，C++ 核心 + Lua 模组 API）

* **对我们的价值**：同为 "方块世界、客户端 / 服务端分离、C++ 实现"，架构层级可直接对照；我们的创新点（三轨时间、复合体刚体、真 LOD）是它没有的 ——**参考它补足 "工业级常规项"，超越它做差异化**



***

## 2. 区块存储：MapBlock 与 Map

### 2.1 MapBlock：16³ 立方体单元（与我们的方案完全一致）



```
// src/mapblock.h
static const u32 ystride = MAP_BLOCKSIZE;              // 16
static const u32 zstride = MAP_BLOCKSIZE * MAP_BLOCKSIZE; // 256
static const u32 nodecount = MAP_BLOCKSIZE * MAP_BLOCKSIZE * MAP_BLOCKSIZE; // 4096
// 索引：data[z * zstride + y * ystride + x]
```



* 单元 = **16×16×16**（`MAP_BLOCKSIZE`），4096 节点，索引 z-major

* 定位：`getNodeBlockPos(p) = getContainerPos(p, MAP_BLOCKSIZE)`—— 即坐标 >>4 找区块、&15 找块内，与我们讨论的定位公式同一思想

* `getBox()` = 区块世界包围盒（`pos_relative ~ pos_relative + 15`）

* **缓存加速**：`m_pos_relative` 预计算（避免每次 3 次乘法）

> 结论：16³ 立方体在工业级引擎中被验证，不是我们的拍脑袋。垂直维度独立（非 MC 的 16×16×256 柱）在 Minetest 也一样 —— 浮空 / 洞穴地形天然需要。

### 2.2 Monoblock：同质块压缩（内存优化，我们的空 chunk 是它的特例）



```
// src/mapblock.h
// 整块所有方块相同时，只存 1 个节点（4 字节），读时命中索引 0
inline MapNode getNode(...) { return data[m_is_mono_block ? 0 : z*zstride + y*ystride + x]; }
void tryShrinkNodes();   // 检测全同质 → 压缩成 monoblock
void expandNodesIfNeeded(); // 写不同方块 → 展开回完整数组
```



* 一块完整数组 = `4096 × sizeof(MapNode)`（MapNode 2 字节 → **8KB**）

* Monoblock = 4 字节 ——**整块同质时内存降 2000 倍**

* 我们的 `unordered_map<ChunkPos, ChunkData>`（空 chunk 不存）是 "全空气特例" 版本；Minetest 的 monoblock 是 "任意同质" 版本（含全石头等）

* **启示**：如果我们的主世界大量区域是单一材质（比如一大片石头山），可以留一个 "同质压缩" 后门；原型阶段空 chunk 不存已够

### 2.3 修改跟踪：变更事件流的服务端种子（重点借鉴）



```
// src/mapblock.h
enum ModReason : u32 {   // 修改原因位掩码
    MOD_REASON_SET_NODE = 1 << 4,        // 方块被改动
    MOD_REASON_REPORT_META_CHANGE = 1 << 6, // 元数据变更
    MOD_REASON_BLOCK_EXPIRED = 1 << 8,   // 卸载
    ...
};
void raiseModified(u32 mod, u32 reason) {
    if (mod > m_modified) { m_modified = mod; m_modified_reason = reason; ... }
    else if (mod == m_modified) m_modified_reason |= reason;  // 原因按位或累积
}
// 修改状态机：CLEAN → WRITE_AT_UNLOAD（卸载时写盘）→ WRITE_NEEDED（需要写盘）
// setNode() 内部自动调 raiseModified(MOD_STATE_WRITE_NEEDED, MOD_REASON_SET_NODE)
```



* **每次 setNode 自动标记 "改了什么、为什么改"**—— 不需要业务代码另开一套脏标记

* 修改状态分级：CLEAN / WRITE\_AT\_UNLOAD / WRITE\_NEEDED（磁盘写优先级）

* **对我们的直接映射**：我们要做的 "变更事件流（第四项）"= 把 Minetest 的 `raiseModified` 从 "服务端本地标记" 升级为 "跨线程事件"—— 服务端区块存储就该带这个修改跟踪，渲染同步时直接读 "本 tick 哪些 chunk 脏了"（按 chunk 聚合，正合我们 `(ChunkPos, 方块, 类型)` 事件形状）

### 2.4 稀疏容器：MapSector（XZ 柱 + Y 列表）



```
// src/map.h
MapSector * getSectorNoGenerate(v2s16 p2d);   // 按 XZ 二维找柱
MapBlock * getBlockNoCreateNoEx(v3s16 p);     // 找不到返回 NULL（不抛异常）
// ClientMap / ServerMap 继承 Map：emergeBlock 各自实现（服务器生成、客户端只读）
```



* Map 按 **XZ 柱（MapSector，16×16）** 组织，Y 方向内是块数组 ——2 级索引

* 客户端（ClientMap）与服务器（ServerMap）继承同一 Map，**读取 / 生成语义分开**（服务器才 emerge，客户端只取已有块）—— 与我们的 "业务权威 / 渲染只读" 分工一致

* 我们的 `unordered_map<ChunkPos, ChunkData>` 是它的简化（哈希直达，省掉 2 级），原型足够

### 2.5 懒缓存：isAir /contents/solid\_sides



```
// src/mapblock.h
bool isAir();                  // m_is_air 懒计算缓存（expireIsAirCache 延迟到用时才重算）
std::vector<content_t> contents; // 内容类型集合缓存（ABM/更新优化用）
// 客户端版成员：
MapBlockMesh *mesh = nullptr;  // 该块的 mesh 缓存（渲染直接挂这里）
u8 solid_sides = 0;            // 六面不透明标记（00 +Z -Z +Y -Y +X -X，遮挡剔除用）
```



* 服务端：isAir /contents 缓存服务 "跳过空气块"（mesh 生成、网络同步都大量用）

* 客户端：**每个 MapBlock 挂一个 mesh 指针**——mesh 缓存粒度 = 区块粒度；`solid_sides` 六面不透明位图供渲染期快速剔除

### 2.6 序列化：disk vs network 两套格式



```
void serialize(std::ostream &result, u8 version, bool disk, int compression_level);
// disk=true → 磁盘格式（含 id-name 映射）；disk=false → 网络格式（压缩）
```



* 同一 MapBlock，磁盘 / 网络两套序列化（版本号版本化）

* 网络格式 zlib 压缩 —— 区块传输不逐方块裸传

* 我们的 SyncBlockSet 原型是 "整世界全量"；区块化后天然演进为 "变更 chunk 集合 + 压缩"



***

## 3. 客户端 Mesh 生成管线（LOD 适配的核心参照）

### 3.1 触发：变更 → 重建该块 mesh + 邻居 mesh



```
// src/client/mesh_generator_thread.cpp
MeshUpdateManager::updateBlock(Map *map, v3s16 p, bool ack, bool urgent, bool update_neighbors) {
    m_queue_in.addBlock(map, p, ack, urgent, false);
    if (update_neighbors) {
        // 平滑光照开：26 邻居；否则 6 邻居
        for (v3s16 dp : g_26dirs / g_6dirs)
            m_queue_in.addBlock(map, p + dp, false, urgent, true);
    }
}
```



* **方块变更 → 该块 mesh 重建 + 邻居块 mesh 重建**（边界方块影响邻居的边界面）

* 26 邻居 vs 6 邻居：平滑光照需要对角邻居的光照信息

* **直接映射我们的讨论**："chunk 边界方块变更要重建邻居 chunk 的 mesh"—— 这是工业级标准做法，不是过度设计

### 3.2 队列：去重 /urgent 优先级 /inflight 防重



```
// 去重：同 mesh_position 已在队列 → 只更新数据不重复入队
// urgent：玩家附近先做（m_urgents 集合，pop 时优先）
// inflight：m_inflight_blocks 记录正在处理的块，防两个 worker 撞同一块
```



* 三个机制我们将来都要（渲染线程落地时）：去重（一 tick 多次变更只重建一次）、优先级（玩家视锥内优先）、防重（多 worker 安全）

### 3.3 数据准备：retrieveBlocks 带边距 + VoxelManipulator 烘焙



```
void QueuedMeshUpdate::retrieveBlocks(Map *map, u16 cell_size) {
    const size_t total = (cell_size+2)*(cell_size+2)*(cell_size+2); // 注意 +2 边距！
    for (pos.X = p.X-1; pos.X <= p.X+cell_size; pos.X++)
      for (pos.Z = p.Z-1; ...) for (pos.Y = p.Y-1; ...)
        map_blocks[i] = map->getBlockNoCreateNoEx(pos);  // 拉邻居块
}
// fillDataFromMapBlocks：把 (cell_size+2)³ 块 copyTo 一个 VoxelManipulator（3D 体素缓冲）
// 然后 MapBlockMesh 从 vmanip 统一烘焙
```



* **mesh 生成拉 (cell\_size+2)³ 个块 = 目标块 + 一圈边距邻居**—— 跨块边界的面、光照才能正确

* VoxelManipulator = 带边距的体素工作缓冲 ——**"从 3D 数组烘焙 mesh" 的通用模式**（我们的 `forEachBlockInAABB` 就服务于这个：渲染端想烘焙时，扩展 1 格查询即得邻居数据）

### 3.4 烘焙：面剔除三规则



```
// src/client/content_mapblock.cpp, MapblockMeshGenerator::drawSolidNode
for (int face = 0; face < 6; face++) {
    p2 = p1 + tile_dirs[face];
    n2 = vmanip.getNodeRefUnsafeCheckFlags(p2);
    if (n2 == n1) continue;            // 规则1：邻居同类型 → 不生成该面（内部面）
    if (n2 == CONTENT_IGNORE) continue; // 规则2：越界/未加载 → 不生成（防边缝）
    if (n2 != AIR && NDT_solidness[f2] == 2) continue; // 规则3：实心邻居 → 背向面剔除
    add_face(face, neighbor, backface_culling);
}
```



* **CONTENT\_IGNORE 哨兵**：vmanip 边界外返回 IGNORE（不是空气！）—— 面生成跳过 → **区块边界的缝就是这么防的**

* 面剔除三规则 = 面片数上限 6 面，实际平均远低于 —— 我们 LOD1"合并共面面片" 的基础就是这套

* 特殊方块（液体 / 火把 / 告示牌 / 植物）单独 draw 函数 —— 我们原型阶段全走 "实心立方体" 即可

### 3.5 worker 线程池



```
// 默认 25% 系统核心，最多 3 个 worker
number_of_threads = std::min(3U, Thread::getNumberOfProcessors() / 4);
// 每 worker：pop 队列 → 生成 MapBlockMesh → putResult（urgent/normal 两出口）→ 主线程取走挂渲染
```



* mesh 生成是独立线程池，**不阻塞模拟线程**—— 印证我们的 "渲染 = 第 4 轨只读 + LOD 独立线程" 路线

### 3.6 跳过全空气块



```
bool QueuedMeshUpdate::checkSkip(u16 cell_size) {
    for (block : map_blocks) if (block 在目标范围内) all_air &= block->isAir();
    return all_air;  // 全空气 → 该 mesh 直接不生成（"air blocks won't suddenly become visible"）
}
```



* 全空气块不需要 mesh（邻居更新也不会让它突然可见）——**空 chunk 零 mesh 成本**，与我们的空 chunk 不存储互为表里

### 3.7 MeshGrid：mesh 单元 ≥ mapblock（cell\_size 可分组）



```
const v3s16 mesh_position = mesh_grid.getMeshPos(p);   // mesh 位置 = 角块（坐标被 cell_size 整除）
// MeshMakeData 尺寸 = MAP_BLOCKSIZE * mesh_grid.cell_size
// → mesh 单元 = cell_size × 16³（如 2×2×2 个 mapblock 合并成一个 mesh）
```



* **mesh 生成单元可以大于一个区块**（cell\_size 可配置）—— 一个 mesh = N×N×N 个 MapBlock

* **直接映射我们的 LOD 讨论**："每 chunk 独立 LOD 单元 vs N×N 分组"——Minetest 给了第三种答案：**默认单元可调，mesh 粒度与数据块粒度解耦**。我们渲染成熟后可以让 LOD0=1 chunk/mesh、远距离 LOD=4×4 chunk/mesh，只改 cell\_size



***

## 4. LOD 现状：Minetest 没有真 LOD

诚实说明：Minetest **没有降采样 LOD**（所有可见区块都是全分辨率方块网格）。它的 "LOD 替代方案"：



1. 距离裁剪（viewing range 限制可见区块数）

2. 区块级 mesh 缓存（mesh 不变就不重建）

3. solid\_sides 六面不透明标记（遮挡剔除）

4. urgent 优先级（玩家附近先建 mesh）

5. 全空气跳过

**所以 "降采样 LOD（LOD2 体素降采样 / LOD1 共面合并）" 是我们的差异化点，Minetest 帮不了，要参考 Cubyz 或自己做。** 但区块存储层（16³ + 密集数组 + 修改跟踪 + 区域查询）已经为降采样铺好了路。



***

## 5. 与 sandbox-engine 区块存储方案对照



| 维度       | Minetest（工业级参照）                  | sandbox-engine 方案（讨论中）               | 结论                |
| -------- | -------------------------------- | ------------------------------------ | ----------------- |
| 数据单元     | MapBlock 16³                     | chunk 16³ 立方体                        | **一致 ✓**（16³ 被验证） |
| 容器       | MapSector（XZ 柱）+ Y 块数组           | `unordered_map<ChunkPos, ChunkData>` | 我们更简（原型够）         |
| 方块编码     | MapNode 2 字节（内容 id + 光照参数）       | uint8 1 字节 BlockType                 | 我们更瘦；光照将来单独通道     |
| 同质优化     | Monoblock 全同质压缩                  | 空 chunk 不存（空气特例）                     | 同思想，我们留后门         |
| 修改跟踪     | raiseModified 状态机 + 原因掩码         | **变更事件流（第四项，借鉴）**                    | 直接照搬思想            |
| mesh 单元  | cell\_size×16³（可分组）              | LOD 单元（渲染期可分组）                       | 同思想，我们将来用         |
| 邻居数据     | retrieveBlocks 拉 +2 边距           | `forEachBlockInAABB` 扩展 1 格          | 同思想               |
| 边界防缝     | CONTENT\_IGNORE 哨兵               | 区域查询越界返回 "忽略"                        | 同思想               |
| mesh 缓存  | MapBlock::mesh 指针 + solid\_sides | 客户端 LOD 缓存（渲染期）                      | 同思想               |
| 空气优化     | checkSkip 全空气跳过                  | 空 chunk 不生成 mesh                     | 同思想               |
| 生成线程     | worker 线程池（25% 核心）               | LOD 线程（计划）                           | 一致                |
| 变更传播     | updateBlock + 6/26 邻居            | 变更事件流 → chunk 脏标记                    | 我们跨线程版            |
| 复合体 / 飞船 | 无（块世界静态）                         | 刚体方块 = 动态渲染单元                        | **我们超越项**         |
| 真 LOD    | 无（距离裁剪替代）                        | LOD0/1/2 降采样（计划）                     | **我们超越项**         |



***

## 6. 对我们区块存储设计的落地启示（7 条）



1. **16³ 立方体定案**：工业级引擎验证，垂直维度独立，适合浮空岛 / 飞船世界

2. **区块存储必须带修改跟踪**（Minetest raiseModified 思想）：`setBlock` 自动标记 "哪个 chunk、什么原因改的"—— 第四项变更事件流直接读脏标记聚合广播，不另开一套

3. **区域查询接口要支持扩展边距**：`forEachBlockInAABB` 之外，提供 "扩展 1 格" 变体 ——mesh 烘焙 / 边界方块变更重建邻居都要用

4. **越界返回 "忽略" 而非空气**：CONTENT\_IGNORE 哨兵思想 —— 区域查询越界时 LOD 生成跳过该面，防边缝（我们的 AABB 碰撞窄相已有 "严格区间重叠"，这里同样防贴边）

5. **mesh 单元与数据块解耦**：渲染端 LOD 单元 = cell\_size × chunk（可分组），数据层只保证 "区域提取 + 变更追踪"，不锁死渲染粒度

6. **空 chunk 零成本**：不存储 + 不生成 mesh 双零（Minetest 两侧都做了，我们也两侧都做）

7. **复合体不进 chunk 的架构优势再次确认**：Minetest 没有动态复合体所以没有这个烦恼；我们的刚体方块独立渲染单元，正好避开 "Minetest 静态 chunk mesh 缓存" 体系对动态体的不适用



***

## 7. 关键源码文件清单（想深读时按这个路径去 GitHub 找）



| 文件                                     | 内容                                                   |
| -------------------------------------- | ---------------------------------------------------- |
| `src/mapblock.h`                       | MapBlock 定义：16³ 存储、monoblock、修改跟踪、序列化、客户端 mesh 挂载    |
| `src/map.h` / `src/map.cpp`            | Map/MapSector 容器层级、块定位、ClientMap/ServerMap 分工        |
| `src/client/mesh_generator_thread.cpp` | mesh 生成线程池：队列 / 去重 /urgent/inflight/ 边距拉块 /vmanip 烘焙 |
| `src/client/content_mapblock.cpp`      | 面片生成：面剔除三规则、光照、特殊方块绘制                                |
| `src/client/mapblock_mesh.cpp` / `.h`  | MapBlockMesh：从 MeshMakeData（vmanip）烘焙顶点              |
| `src/mapnode.h`                        | MapNode 2 字节编码（内容 id + 参数 / 光照）                      |
| `src/network/networkprotocol.cpp`      | MapBlock 网络序列化与压缩                                    |



***

## 附：本文与项目的关系



* 服务于数据层 ② 剩余项：**区块化存储（设计讨论中）+ 变更事件流（第四项）**

* 结论：我们的方案（16³ / 稀疏映射 / 区域查询 / 复合体不进 chunk / LOD 单元可分组）与工业级参照**方向一致**，可按讨论定案落地；LOD 降采样是差异化部分，渲染成熟期再补

* 参考原则：LGPL-2.1 只读借鉴（不抄代码），延续 Sable/Rapier 的 "只学不抄" 纪律
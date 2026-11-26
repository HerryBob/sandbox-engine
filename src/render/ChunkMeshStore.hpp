#pragma once
#include <cstddef>
#include <unordered_map>

#include "render/MeshTypes.hpp"
#include "shared/chunk/ChunkPos.hpp"

namespace engine::render {

// ============================================================================
// ChunkMeshStore：世界 chunk 的 GPU 网格表 + 按帧槽延迟销毁（P14）
// ============================================================================
//
// 从 VulkanContext 抽出的最后一块职责（S5）。它只管**资源**：
//   · upload(cp, data)：把 CPU 网格数据传到 GPU（空数据 = 删除该 chunk 的旧网格）
//   · setFrameSlot(slot)：由 drawFrame 每帧告知当前帧槽（回收用）
//   · collectRetired(slot)：该帧槽的 fence 已 wait → 上一轮挂在这里的旧缓冲可真正销毁
//   · meshes()：给录制命令遍历
//
// **P14 的安全推导**（kMaxFramesInFlight = 2）：
//   在第 F 帧（槽 S）退休的网格，最后一次被引用是 F-1 帧（槽 1-S）；
//   第 F+2 帧重新轮到槽 S 时，S 的 fence（F 帧）刚 wait 完，而 1-S 的 fence（F-1 帧）
//   在第 F+1 帧已经 wait 过 —— 两者都完成，销毁安全。
//   所以这里**不需要** vkDeviceWaitIdle（原来有，实测每次被拖 6.18ms）。
//
// ---- 未来接缝（不实现，只留位置）----
//   · 世界 LOD：key 从 ChunkPos 扩成 (ChunkPos, lod)；mesh 每级一份，按距离选择绘制。
//   · 环绕复用：meshes_ 的 value 可加 generation，LOD 切换时避免重建。
class ChunkMeshStore {
public:
    static constexpr std::size_t kMaxFramesInFlight = 2;

    ChunkMeshStore() = default;

    bool init(VkDevice device, VkPhysicalDevice physicalDevice);
    void shutdown();

    // 上传/替换某 chunk 的网格。data 为空 → 删除旧网格（地形被挖空/卸载）。
    bool upload(const engine::shared::ChunkPos& cp, const ChunkMeshData& data);

    void setFrameSlot(std::size_t slot) { currentSlot_ = slot; }
    void collectRetired(std::size_t slot);

    const std::unordered_map<engine::shared::ChunkPos, ChunkMesh>& meshes() const {
        return meshes_;
    }
    bool empty() const { return meshes_.empty(); }

private:
    void retire(ChunkMesh&& m);

    VkDevice         device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    std::size_t      currentSlot_ = 0;
    std::unordered_map<engine::shared::ChunkPos, ChunkMesh> meshes_;
    std::vector<ChunkMesh> graveyard_[kMaxFramesInFlight];
};

} // namespace engine::render

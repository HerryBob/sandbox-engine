#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "render/MeshTypes.hpp"

namespace engine::render {

// ============================================================================
// 实体渲染资源（S6b）：模型骨骼网格 + 贴图 + 每帧位姿
// ============================================================================
//
// 从 VulkanContext 抽出的第六块职责。与 BodyMeshStore 同构：
//   资源（网格/贴图）与实例状态（位置/朝向/可见性）分开存，
//   因为"同一份模型可被多个实例共用，隐藏其中一个不该连带隐藏别人"。
//
// 依赖（init 注入）：device / physicalDevice / graphicsQueue / cmdPool
//                   descPool / descLayout / uboBuffer（贴图要占描述符集，binding 0 绑 UBO）
//
// ---- 未来接缝（不实现，只留位置）----
//   · 生物实体 LOD：draw 循环里按距离跳过/降级（本类的 meshes() 只给数据，决定权在录制侧）。
//   · 骨骼动画：EntityGpu::order 已经按拓扑序排好，每帧把局部旋转写进 boneWorld 即可。
//   · 实体 LOD 的贴图分级：ensureTexture() 按路径缓存，加 mip 链即可。

// 一根骨骼的 GPU 网格：与 ChunkMesh 的 Solid 桶同构（实体不需要 Cutout 桶）。
struct EntityBoneGpu {
    VkBuffer       vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer       indexBuffer  = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory  = VK_NULL_HANDLE;
    uint32_t       indexCount   = 0;
};

// 一个实体的全部骨骼资源与骨骼树。
// order 是拓扑序（父一定排在子前面），每帧按它顺序累乘世界矩阵。
struct EntityGpu {
    std::vector<EntityBoneGpu> bones;
    std::vector<int>           parents;   // -1 = 根
    std::vector<glm::vec3>     pivots;    // 模型局部空间（世界单位）
    std::vector<int>           order;
    VkDescriptorSet            textureSet = VK_NULL_HANDLE;   // 该实体用的贴图 descriptor
};

// 实体的每帧状态：位置 + 绕 y 轴朝向（度）+ 可见性。
struct EntityPose {
    glm::dvec3 pos{0.0, 0.0, 0.0};
    float      yawDeg = 0.0f;
    bool       visible = true;
};

// 一张实体贴图及其 descriptor set。按贴图路径建一次，多个实体可共用同一张。
struct EntityTextureRes {
    VkImage         image   = VK_NULL_HANDLE;
    VkDeviceMemory  memory  = VK_NULL_HANDLE;
    VkImageView     view    = VK_NULL_HANDLE;
    VkSampler       sampler = VK_NULL_HANDLE;
    VkDescriptorSet set     = VK_NULL_HANDLE;
};

class EntityRenderer {
public:
    EntityRenderer() = default;

    bool init(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue graphicsQueue,
              VkCommandPool cmdPool, VkDescriptorPool descPool,
              VkDescriptorSetLayout descLayout, VkBuffer uboBuffer);
    void shutdown();   // 销毁所有实体网格 + 贴图

    // 加载一个实体模型：骨骼网格 + 贴图（贴图按路径缓存，多实例共用）。
    bool loadModel(int entityId, const std::string& bbmodelPath, const std::string& texturePath);
    void destroyModel(int entityId);

    // 每帧状态
    void setPose(int entityId, double x, double y, double z, float yawDeg);
    void setVisible(int entityId, bool visible);

    // 只读访问（录制命令用）
    const std::unordered_map<int, EntityGpu>&  meshes() const { return meshes_; }
    const std::unordered_map<int, EntityPose>& poses() const { return poses_; }

private:
    bool ensureTexture(const std::string& path);

    VkDevice              device_ = VK_NULL_HANDLE;
    VkPhysicalDevice      physicalDevice_ = VK_NULL_HANDLE;
    VkQueue               graphicsQueue_ = VK_NULL_HANDLE;
    VkCommandPool         cmdPool_ = VK_NULL_HANDLE;
    VkDescriptorPool      descPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descLayout_ = VK_NULL_HANDLE;
    VkBuffer              uboBuffer_ = VK_NULL_HANDLE;

    std::unordered_map<std::string, EntityTextureRes> textures_;
    std::unordered_map<int, EntityGpu>  meshes_;
    std::unordered_map<int, EntityPose> poses_;
};

} // namespace engine::render

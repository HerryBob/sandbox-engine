#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include "render/EntityRenderer.hpp"

#include <cstring>

// stb_image 的 IMPLEMENTATION 只在 VulkanContext.cpp 里定义一次，这里只声明
#include "stb_image.h"

#include "core/Log.hpp"
#include "render/EntityMesh.hpp"
#include "render/VulkanContext.hpp"     // UniformBufferObject（贴图描述符的 binding 0）
#include "render/assets/BbModelLoader.hpp"
#include "render/utils/CommandUtils.hpp"
#include "render/utils/ResourcesUtils.hpp"

namespace engine::render {

bool EntityRenderer::init(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue graphicsQueue,
                          VkCommandPool cmdPool, VkDescriptorPool descPool,
                          VkDescriptorSetLayout descLayout, VkBuffer uboBuffer) {
    device_ = device;
    physicalDevice_ = physicalDevice;
    graphicsQueue_ = graphicsQueue;
    cmdPool_ = cmdPool;
    descPool_ = descPool;
    descLayout_ = descLayout;
    uboBuffer_ = uboBuffer;
    return true;
}

void EntityRenderer::shutdown() {
    for (auto& [id, gpu] : meshes_) {
        for (auto& g : gpu.bones) {
            if (g.vertexBuffer) vkDestroyBuffer(device_, g.vertexBuffer, nullptr);
            if (g.vertexMemory) vkFreeMemory(device_, g.vertexMemory, nullptr);
            if (g.indexBuffer)  vkDestroyBuffer(device_, g.indexBuffer, nullptr);
            if (g.indexMemory)  vkFreeMemory(device_, g.indexMemory, nullptr);
        }
    }
    meshes_.clear();
    poses_.clear();
    for (auto& [path, res] : textures_) {
        if (res.sampler) vkDestroySampler(device_, res.sampler, nullptr);
        if (res.view)    vkDestroyImageView(device_, res.view, nullptr);
        if (res.image)   vkDestroyImage(device_, res.image, nullptr);
        if (res.memory)  vkFreeMemory(device_, res.memory, nullptr);
        // descriptor set 由池统一释放，不必单独 free
    }
    textures_.clear();
}
bool EntityRenderer::loadModel(int entityId, const std::string& bbmodelPath,
                                    const std::string& texturePath) {
    engine::assets::EntityModel model;
    if (!engine::assets::loadBbModel(bbmodelPath, model))
        return false;

    // 贴图路径由实体类型定义给出；空串表示"由模型文件自己寻址"。
    // 模型自寻址（bbmodel 内嵌 base64 导出）还没做，因此空串时直接不加载贴图，
    // 该实体不绘制——渲染层不再假设"默认玩家贴图"这种东西。
    const std::string texPath = texturePath;

    const mesh::EntityMeshData data = mesh::buildEntityVertices(model);
    if (data.bones.size() != model.bones.size())
        return false;

    EntityGpu gpu;
    gpu.bones.resize(model.bones.size());
    gpu.parents.resize(model.bones.size(), -1);
    gpu.pivots.resize(model.bones.size(), glm::vec3(0.0f));

    for (std::size_t i = 0; i < data.bones.size(); ++i) {
        const auto& bm = data.bones[i];
        gpu.parents[i] = model.bones[i].parent;
        gpu.pivots[i] = model.bones[i].pivot * 0.0625f;   // 16 单位 → 世界单位
        if (bm.vertices.empty() || bm.indices.empty()) continue;

        EntityBoneGpu& g = gpu.bones[i];
        if (!util::createBuffer(device_, physicalDevice_,
                sizeof(Vertex) * bm.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                g.vertexBuffer, g.vertexMemory)) return false;
        util::mapAndCopy(device_, g.vertexMemory, sizeof(Vertex) * bm.vertices.size(), bm.vertices.data());

        if (!util::createBuffer(device_, physicalDevice_,
                sizeof(std::uint32_t) * bm.indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                g.indexBuffer, g.indexMemory)) return false;
        util::mapAndCopy(device_, g.indexMemory, sizeof(std::uint32_t) * bm.indices.size(), bm.indices.data());

        g.indexCount = static_cast<std::uint32_t>(bm.indices.size());
    }

    // 拓扑序：父必须排在子前面，每帧按这个顺序从根往下累乘世界矩阵
    std::vector<std::vector<int>> children(model.bones.size());
    std::vector<int> indeg(model.bones.size(), 0);
    for (std::size_t i = 0; i < model.bones.size(); ++i) {
        const int p = gpu.parents[i];
        if (p >= 0) { children[p].push_back(static_cast<int>(i)); indeg[i] = 1; }
    }
    for (std::size_t i = 0; i < model.bones.size(); ++i)
        if (indeg[i] == 0) gpu.order.push_back(static_cast<int>(i));
    for (std::size_t k = 0; k < gpu.order.size(); ++k)
        for (int c : children[gpu.order[k]]) gpu.order.push_back(c);

    // 贴图：加载失败则 textureSet 保持空，绘制趟跳过这个实体（不影响方块）
    if (ensureTexture(texPath)) {
        gpu.textureSet = textures_[texPath].set;
    } else {
        ENGINE_LOG_WARN("Vulkan", "实体 id=" + std::to_string(entityId) + " 无可用贴图，将不绘制");
    }

    const std::size_t boneCount = gpu.bones.size();
    meshes_[entityId] = std::move(gpu);
    poses_[entityId] = EntityPose{};

    ENGINE_LOG_INFO("Vulkan", "实体网格 id=" + std::to_string(entityId)
        + " 骨骼=" + std::to_string(boneCount));
    return true;
}

void EntityRenderer::setPose(int entityId, double x, double y, double z, float yawDeg) {
    EntityPose& p = poses_[entityId];
    p.pos = glm::dvec3{x, y, z};
    p.yawDeg = yawDeg;
}

// 可见性写在实例状态（poses_）而不是资源（meshes_）：
// 同一份网格可能被多个实例共用，隐藏其中一个不该连带隐藏别人。
void EntityRenderer::setVisible(int entityId, bool visible) {
    auto it = poses_.find(entityId);
    if (it != poses_.end()) it->second.visible = visible;
}

void EntityRenderer::destroyModel(int entityId) {
    auto it = meshes_.find(entityId);
    if (it != meshes_.end()) {
        for (auto& g : it->second.bones) {
            if (g.vertexBuffer) vkDestroyBuffer(device_, g.vertexBuffer, nullptr);
            if (g.vertexMemory) vkFreeMemory(device_, g.vertexMemory, nullptr);
            if (g.indexBuffer)  vkDestroyBuffer(device_, g.indexBuffer, nullptr);
            if (g.indexMemory)  vkFreeMemory(device_, g.indexMemory, nullptr);
        }
        meshes_.erase(it);
    }
    poses_.erase(entityId);
}

bool EntityRenderer::ensureTexture(const std::string& path) {
    // 空路径 = 类型定义没给贴图，也没做模型自寻址：直接判无贴图，不占描述符额度。
    if (path.empty()) return false;

    auto it = textures_.find(path);
    if (it != textures_.end()) return it->second.set != VK_NULL_HANDLE;

    EntityTextureRes res;

    // 先占一个 descriptor set：池里按 kMaxEntityTextures 预留了额度
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = descPool_;
    setAlloc.descriptorSetCount = 1;
    VkDescriptorSetLayout atlasLayout = descLayout_;
    setAlloc.pSetLayouts = &atlasLayout;
    if (vkAllocateDescriptorSets(device_, &setAlloc, &res.set) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "实体贴图描述符集分配失败：" + path);
        return false;
    }

    int w = 0, h = 0, ch = 0;
    stbi_uc* src = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!src) {
        ENGINE_LOG_WARN("Vulkan", "实体贴图读不到：" + path + "（引用它的实体不绘制）");
        return false;
    }

    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * 4;
    if (!util::createImage(device_, physicalDevice_,
            static_cast<uint32_t>(w), static_cast<uint32_t>(h), VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            1, VK_SAMPLE_COUNT_1_BIT, res.image, res.memory)) {
        stbi_image_free(src);
        ENGINE_LOG_ERROR("Vulkan", "实体贴图图像创建失败：" + path);
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (!util::createBuffer(device_, physicalDevice_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging, stagingMem)) {
        stbi_image_free(src);
        return false;
    }
    void* data = nullptr;
    vkMapMemory(device_, stagingMem, 0, size, 0, &data);
    std::memcpy(data, src, static_cast<std::size_t>(size));
    vkUnmapMemory(device_, stagingMem);
    stbi_image_free(src);

    util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, res.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    util::copyBufferToImage(device_, graphicsQueue_, cmdPool_, staging, res.image,
        static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, res.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);

    if (!util::createImageView(device_, res.image, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_ASPECT_COLOR_BIT, res.view)) {
        ENGINE_LOG_ERROR("Vulkan", "实体贴图视图创建失败：" + path);
        return false;
    }

    // 像素画必须最近邻；CLAMP 防边缘泄漏
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.maxLod = 1.0f;
    if (vkCreateSampler(device_, &si, nullptr, &res.sampler) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "实体采样器创建失败：" + path);
        return false;
    }

    // binding 0 = 与方块共用的同一个 UBO；binding 1 = 这张贴图
    VkDescriptorBufferInfo binfo{ uboBuffer_, 0, sizeof(UniformBufferObject) };
    VkDescriptorImageInfo  ii{ res.sampler, res.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr[2]{};
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[0].dstSet = res.set;
    wr[0].dstBinding = 0;
    wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wr[0].pBufferInfo = &binfo;
    wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[1].dstSet = res.set;
    wr[1].dstBinding = 1;
    wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[1].pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 2, wr, 0, nullptr);

    textures_[path] = res;
    ENGINE_LOG_INFO("Vulkan", "实体贴图就绪：" + path + "（" + std::to_string(w) + "x" + std::to_string(h) + "）");
    return true;
}

// ---- GLFW 输入回调（static 成员定义；经 user pointer 转发到 this）----
// 描述符集：UBO（binding 0）+ 纹理 sampler（binding 1）
} // namespace engine::render

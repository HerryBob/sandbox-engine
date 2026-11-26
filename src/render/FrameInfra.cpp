#include "render/FrameInfra.hpp"

#include "render/VulkanContext.hpp"   // 只为 UniformBufferObject 定义

#include <sstream>

#include "core/Log.hpp"
#include "render/utils/ResourcesUtils.hpp"

namespace engine::render {

bool FrameInfra::initCommandAndSync(VkDevice device, VkPhysicalDevice physicalDevice,
                                   VkQueue graphicsQueue, uint32_t graphicsQueueFamily,
                                   uint32_t swapchainImageCount) {
    device_ = device;
    physicalDevice_ = physicalDevice;
    graphicsQueue_ = graphicsQueue;
    graphicsQueueFamily_ = graphicsQueueFamily;
    swapchainImageCount_ = swapchainImageCount;
    if (!createCommandObjects()) return false;   // 命令池必须先有：纹理上传要用它
    if (!createSyncObjects()) return false;
    return true;
}

bool FrameInfra::initDescriptors(VkImageView atlasView, VkSampler atlasSampler) {
    atlasView_ = atlasView;
    atlasSampler_ = atlasSampler;
    return createUniformObjects();               // UBO + 描述符布局/池/集（bind 图集）
}

void FrameInfra::shutdown() {
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (inFlightFences_[i]) { vkDestroyFence(device_, inFlightFences_[i], nullptr); inFlightFences_[i] = VK_NULL_HANDLE; }
        if (imageAvailable_[i]) { vkDestroySemaphore(device_, imageAvailable_[i], nullptr); imageAvailable_[i] = VK_NULL_HANDLE; }
    }
    for (auto& s : renderFinished_) if (s) vkDestroySemaphore(device_, s, nullptr);
    renderFinished_.clear();
    destroyUniformObjects();
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (cmdBuffers_[i]) { vkFreeCommandBuffers(device_, cmdPool_, 1, &cmdBuffers_[i]); cmdBuffers_[i] = VK_NULL_HANDLE; }
    }
    if (cmdPool_) { vkDestroyCommandPool(device_, cmdPool_, nullptr); cmdPool_ = VK_NULL_HANDLE; }
}
bool FrameInfra::createCommandObjects() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // 允许每帧 reset
    poolInfo.queueFamilyIndex = graphicsQueueFamily_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &cmdPool_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkCreateCommandPool 失败");
        return false;
    }
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = cmdPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kMaxFramesInFlight;
    if (vkAllocateCommandBuffers(device_, &allocInfo, cmdBuffers_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkAllocateCommandBuffers 失败");
        return false;
    }
    ENGINE_LOG_INFO("Vulkan", "命令池 + 命令缓冲就绪（每帧槽一条，2 条轮转）");
    return true;
}

// 纹理图像：staging 搬运（构建图集 → 上传显存 → 转 SHADER_READ_ONLY）
// 图集 = 一张大纹理装所有方块小纹理（每格 16²，边长由声明条目数算出）


 

// 纹理图像 → 可采样视图

// 创建采样器（线性过滤 + REPEAT 寻址）


// 确保某张实体贴图已加载：图像 + 视图 + 最近邻采样器 + 自己的 descriptor set。
// 按路径缓存——同一张贴图被多个实体类型引用时只建一次。

bool FrameInfra::createUniformObjects() {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = sizeof(UniformBufferObject);
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &uboBuffer_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "UBO vkCreateBuffer 失败");
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, uboBuffer_, &req);
    uint32_t memIdx = util::findMemoryType(physicalDevice_, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memIdx == UINT32_MAX) {
        ENGINE_LOG_ERROR("Vulkan", "UBO 无 host 可见内存类型");
        return false;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(device_, &ai, nullptr, &uboMemory_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "UBO vkAllocateMemory 失败");
        return false;
    }
    vkBindBufferMemory(device_, uboBuffer_, uboMemory_, 0);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; // 只有顶点着色器用 MVP
    VkDescriptorSetLayoutBinding samplerBinding{};     // ← 新增
    samplerBinding.binding = 1;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding bindings[2] = { binding, samplerBinding };
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descLayout_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkCreateDescriptorSetLayout 失败");
        return false;
    }

    // 一套 set 的组成：binding 0 = UBO，binding 1 = 贴图。
    // 额度 = 方块图集 1 套 + 实体贴图各 1 套。实体贴图数量上限 6 张，
    // 超出的 ensureEntityTexture 会在分配 set 时报错返回 false（不是崩溃）。
    constexpr uint32_t kMaxEntityTextures = 6;
    VkDescriptorPoolSize poolSizes[2] = {
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 + kMaxEntityTextures},
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 + kMaxEntityTextures},   // 图集 + 各实体贴图
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1 + kMaxEntityTextures;         // 方块一套 + 每个实体贴图一套
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descPool_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkCreateDescriptorPool 失败");
        return false;
    }

    // 方块图集那一套在这里一次性分配；实体贴图的 set 由 ensureEntityTexture 按需分配
    VkDescriptorSetLayout setLayouts[1] = { descLayout_ };
    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = descPool_;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = setLayouts;
    VkDescriptorSet sets[1]{};
    if (vkAllocateDescriptorSets(device_, &setAllocInfo, sets) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkAllocateDescriptorSets 失败");
        return false;
    }
    descSet_ = sets[0];        // 方块图集那套

    VkDescriptorBufferInfo binfo{uboBuffer_, 0, sizeof(UniformBufferObject)};
    VkDescriptorImageInfo iinfo{ atlasSampler_, atlasView_,
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };       // ← 新增：采样器+视图
    VkWriteDescriptorSet write[2]{};
    write[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write[0].dstSet = descSet_; 
    write[0].dstBinding = 0;   // 现有 UBO 写
    write[0].descriptorCount = 1; 
    write[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write[0].pBufferInfo = &binfo;
    write[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write[1].dstSet = descSet_; 
    write[1].dstBinding = 1;   // ← 新增 sampler 写
    write[1].descriptorCount = 1; 
    write[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write[1].pImageInfo = &iinfo;
    vkUpdateDescriptorSets(device_, 2, write, 0, nullptr);

    ENGINE_LOG_INFO("Vulkan", "MVP uniform 就绪（descriptor set + UBO，相机可操作）");
    return true;
}

// 同步对象：acquire/renderFinished 信号量 + 栅栏（每帧轮转）

bool FrameInfra::createSyncObjects() {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // 初始已 signaled：第一帧 wait 直接过

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("Vulkan", "同步对象创建失败");
            return false;
        }
    }
    renderFinished_.resize(swapchainImageCount_, VK_NULL_HANDLE);
    for (auto& sem : renderFinished_) {
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &sem) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("Vulkan", "renderFinished 信号量创建失败");
            return false;
        }
    }
    ENGINE_LOG_INFO("Vulkan", "同步对象就绪（acquire×2 轮转 + renderFinished×图像数 + 栅栏×2）");
    return true;
}

// 每帧从 CameraState 读相机状态 → 计算 MVP → 写 UBO。
// 相机状态由渲染线程（每帧调 InputSampler::tick）维护；本函数只读，不再处理输入。

void FrameInfra::destroyUniformObjects() {
    if (descPool_) { vkDestroyDescriptorPool(device_, descPool_, nullptr); descPool_ = VK_NULL_HANDLE; }
    if (descLayout_) { vkDestroyDescriptorSetLayout(device_, descLayout_, nullptr); descLayout_ = VK_NULL_HANDLE; }
    if (uboBuffer_) { vkDestroyBuffer(device_, uboBuffer_, nullptr); uboBuffer_ = VK_NULL_HANDLE; }
    if (uboMemory_) { vkFreeMemory(device_, uboMemory_, nullptr); uboMemory_ = VK_NULL_HANDLE; }
}





// 逆创建序释放全部渲染对象
} // namespace engine::render

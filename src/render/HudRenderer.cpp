#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include "render/HudRenderer.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

// stb_image 的 IMPLEMENTATION 只在 VulkanContext.cpp 里定义一次，这里只声明
#include "stb_image.h"

#include "core/Log.hpp"
#include "render/utils/CommandUtils.hpp"
#include "render/utils/ResourcesUtils.hpp"
#include "render/utils/ShaderUtils.hpp"

namespace engine::render {
// init/shutdown 适配层：HudRenderer 自己保存 init 时传入的句柄，
// 之后 createFontTexture / createHudPipeline / destroy* 都用它们，不再向 VulkanContext 要。
bool HudRenderer::init(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue graphicsQueue,
                       VkCommandPool cmdPool, VkRenderPass renderPass, VkExtent2D extent,
                       VkSampleCountFlagBits msaaSamples) {
    physicalDevice_ = physicalDevice;
    device_         = device;
    graphicsQueue_  = graphicsQueue;
    cmdPool_        = cmdPool;
    renderPass_     = renderPass;
    swapchainExtent_ = extent;
    msaaSamples_    = msaaSamples;
    return createHudResources();
}

void HudRenderer::shutdown() {
    destroyHudResources();
}

// ============================================================================
// HUD（游戏内调试浮层 / F3）
// 与 3D 双管线完全隔离：独立 pipelineLayout（只挂字体纹理）、关深度、alpha 混合，
// 画在同一个 renderPass 的最后一趟（Cutout 之后、EndRenderPass 之前）。
// ============================================================================

// 可见性切换（渲染线程调用）：打日志是为了让"F3 到底有没有送达/有没有被消费"可验证，
// 不然 HUD 不显示时分不清是没切换还是绘制出问题。
void HudRenderer::setHudVisible(bool visible) {
    hudVisible_ = visible;
    ENGINE_LOG_INFO("Vulkan", visible ? "[HUD] 显示（F3）" : "[HUD] 隐藏（F3）");
}

// 业务统计喂入：由渲染线程按 1Hz 调用。
// 为什么不用每帧：chunkCount()/blockCount() 内部要 lock_guard(blocksMutex_)，
// 每帧查等于让渲染帧率去和业务 tick 抢同一把锁。
// tps 由相邻两次采样的 tickCount 差分 / 墙钟差分估算（两次采样间隔就是刷新周期）。
void HudRenderer::setHudStats(long long chunkCount, long long blockCount,
                                long long tickCount, double simTime) {
    const auto now = std::chrono::steady_clock::now();
    if (hudInfo_.valid) {
        const double dtSec = std::chrono::duration<double>(now - tpsWindowStart_).count();
        if (dtSec > 0.05) {
            hudInfo_.tps = static_cast<double>(tickCount - tpsLastTicks_) / dtSec;
        }
    }
    tpsWindowStart_ = now;
    tpsLastTicks_ = tickCount;

    hudInfo_.chunkCount = chunkCount;
    hudInfo_.blockCount = blockCount;
    hudInfo_.tickCount = tickCount;
    hudInfo_.simTime = simTime;
    hudInfo_.valid = true;
}

// 字体图集：assets/textures/font/ascii.png（由 tools/gen_font_atlas.ps1 离线生成）
// 黑底白字，R 通道 = 覆盖率；第 0 格是纯白块（画背板用）。
bool HudRenderer::createFontTexture() {
    int w = 0, h = 0, ch = 0;
    stbi_uc* src = stbi_load("assets/textures/font/ascii.png", &w, &h, &ch, STBI_rgb_alpha);
    if (!src) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 字体图集缺失：assets/textures/font/ascii.png"
                  "（运行 tools/gen_font_atlas.ps1 生成）");
        return false;
    }
    // 尺寸必须与代码常量一致，否则所有 UV 都会错位 → 宁可明确报错也不要画乱码
    if (w != hud::kAtlasW || h != hud::kAtlasH) {
        std::ostringstream os;
        os << "[HUD] 字体图集尺寸 " << w << "x" << h
           << " 与代码常量 " << hud::kAtlasW << "x" << hud::kAtlasH
           << " 不一致（重跑 tools/gen_font_atlas.ps1）";
        ENGINE_LOG_ERROR("Vulkan", os.str());
        stbi_image_free(src);
        return false;
    }

    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(w) * h * 4;
    if (!util::createImage(device_, physicalDevice_,
            static_cast<uint32_t>(w), static_cast<uint32_t>(h), VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            1, VK_SAMPLE_COUNT_1_BIT, fontImage_, fontImageMemory_)) {
        stbi_image_free(src);
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (!util::createBuffer(device_, physicalDevice_, imageSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging, stagingMem)) {
        stbi_image_free(src);
        return false;
    }
    void* data = nullptr;
    vkMapMemory(device_, stagingMem, 0, imageSize, 0, &data);
    std::memcpy(data, src, static_cast<std::size_t>(imageSize));
    vkUnmapMemory(device_, stagingMem);
    stbi_image_free(src);

    util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, fontImage_,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    util::copyBufferToImage(device_, graphicsQueue_, cmdPool_, staging, fontImage_,
        static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, fontImage_,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);

    if (!util::createImageView(device_, fontImage_, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_ASPECT_COLOR_BIT, fontImageView_)) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 字体纹理视图创建失败");
        return false;
    }

    // 采样器：NEAREST 必须（像素字体做线性插值会糊成一团），CLAMP 防止 UV 边缘渗到邻格
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device_, &samplerInfo, nullptr, &fontSampler_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 字体采样器创建失败");
        return false;
    }

    std::ostringstream os;
    os << "[HUD] 字体图集已上传 " << w << "x" << h;
    ENGINE_LOG_INFO("Vulkan", os.str());
    return true;
}

// HUD 管线：2D 四边形，关深度、开 alpha 混合、cull NONE。
// 必须用 msaaSamples_ —— 同一个 renderPass 里所有管线采样数必须一致。
bool HudRenderer::createHudPipeline() {
    VkShaderModule vert = util::loadShaderModule(device_, "shaders/hud_vert.spv");
    if (vert == VK_NULL_HANDLE) return false;
    VkShaderModule frag = util::loadShaderModule(device_, "shaders/hud_frag.spv");
    if (frag == VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, vert, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(hud::HudVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // 与 shaders/hud.vert 的 location 0/1/2 一一对应
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;        // vec2 pos（像素）
    attrs[0].offset = static_cast<uint32_t>(offsetof(hud::HudVertex, pos));
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;        // vec2 uv
    attrs[1].offset = static_cast<uint32_t>(offsetof(hud::HudVertex, uv));
    attrs[2].location = 2;
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;  // vec4 color
    attrs[2].offset = static_cast<uint32_t>(offsetof(hud::HudVertex, color));

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // 动态 viewport/scissor：直接复用 drawFrame 里已经设置好的整窗视口
    VkViewport viewport{};
    VkRect2D scissor{};
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dyn;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;   // 2D 四边形也参与 3D 面剔除没有意义
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    rasterizer.lineWidth = 1.0f;

    // 关深度：HUD 永远覆盖在 3D 画面上，不读也不写深度缓冲
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = msaaSamples_;

    // alpha 混合：背板半透明压暗背景，字形按覆盖率叠加
    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttach.blendEnable = VK_TRUE;
    blendAttach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttach.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttach.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttach;

    // push constant = vec2 屏幕尺寸（顶点阶段换算 NDC；resize 只改参数，不重建管线）
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(float) * 2;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &hudDescLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &hudPipelineLayout_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 管线布局创建失败");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = hudPipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                nullptr, &hudPipeline_);
    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "[HUD] vkCreateGraphicsPipelines 失败 result=" << r;
        ENGINE_LOG_ERROR("Vulkan", os.str());
        return false;
    }
    ENGINE_LOG_INFO("Vulkan", "[HUD] 管线创建成功（depth off / alpha blend / MSAA 匹配主场景）");
    return true;
}

// HUD 全部 GPU 资源：字体纹理 → 描述符 → 管线 → 每帧顶点缓冲。
bool HudRenderer::createHudResources() {
    if (!createFontTexture()) return false;

    // 独立描述符布局/池：只放一张字体纹理，不和 3D 的 UBO 共享额度
    VkDescriptorSetLayoutBinding fontBinding{};
    fontBinding.binding = 0;
    fontBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    fontBinding.descriptorCount = 1;
    fontBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &fontBinding;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &hudDescLayout_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 描述符布局创建失败");
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &hudDescPool_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 描述符池创建失败");
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = hudDescPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &hudDescLayout_;
    if (vkAllocateDescriptorSets(device_, &allocInfo, &hudDescSet_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 描述符集分配失败");
        return false;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = fontImageView_;
    imageInfo.sampler = fontSampler_;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = hudDescSet_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    if (!createHudPipeline()) return false;

    // 顶点缓冲：每帧槽一份，host visible + coherent、持久映射（每帧只 memcpy 不 flush）
    const VkDeviceSize bufSize = static_cast<VkDeviceSize>(sizeof(hud::HudVertex)) * kHudVertexCapacity;
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (!util::createBuffer(device_, physicalDevice_, bufSize,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                hudVertexBuffer_[i], hudVertexMemory_[i])) {
            ENGINE_LOG_ERROR("Vulkan", "[HUD] 顶点缓冲创建失败");
            return false;
        }
        void* mapped = nullptr;
        if (vkMapMemory(device_, hudVertexMemory_[i], 0, bufSize, 0, &mapped) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("Vulkan", "[HUD] 顶点缓冲映射失败");
            return false;
        }
        hudVertexMapped_[i] = mapped;
    }
    ENGINE_LOG_INFO("Vulkan", "[HUD] 资源就绪（顶点容量 12288 = 2048 字符 × 6）");
    return true;
}

// 逆序释放（可重复调用：每个句柄销毁后立刻置空）
void HudRenderer::destroyHudResources() {
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (hudVertexMapped_[i] && hudVertexMemory_[i]) {
            vkUnmapMemory(device_, hudVertexMemory_[i]);
            hudVertexMapped_[i] = nullptr;
        }
        if (hudVertexBuffer_[i]) { vkDestroyBuffer(device_, hudVertexBuffer_[i], nullptr); hudVertexBuffer_[i] = VK_NULL_HANDLE; }
        if (hudVertexMemory_[i]) { vkFreeMemory(device_, hudVertexMemory_[i], nullptr); hudVertexMemory_[i] = VK_NULL_HANDLE; }
    }
    if (hudPipeline_) { vkDestroyPipeline(device_, hudPipeline_, nullptr); hudPipeline_ = VK_NULL_HANDLE; }
    if (hudPipelineLayout_) { vkDestroyPipelineLayout(device_, hudPipelineLayout_, nullptr); hudPipelineLayout_ = VK_NULL_HANDLE; }
    if (hudDescPool_) { vkDestroyDescriptorPool(device_, hudDescPool_, nullptr); hudDescPool_ = VK_NULL_HANDLE; hudDescSet_ = VK_NULL_HANDLE; }
    if (hudDescLayout_) { vkDestroyDescriptorSetLayout(device_, hudDescLayout_, nullptr); hudDescLayout_ = VK_NULL_HANDLE; }
    if (fontSampler_) { vkDestroySampler(device_, fontSampler_, nullptr); fontSampler_ = VK_NULL_HANDLE; }
    if (fontImageView_) { vkDestroyImageView(device_, fontImageView_, nullptr); fontImageView_ = VK_NULL_HANDLE; }
    if (fontImage_) { vkDestroyImage(device_, fontImage_, nullptr); fontImage_ = VK_NULL_HANDLE; }
    if (fontImageMemory_) { vkFreeMemory(device_, fontImageMemory_, nullptr); fontImageMemory_ = VK_NULL_HANDLE; }
    hudText_.clear();
}

// 组装本帧 HUD 文本 → 顶点流。
// 两档数据来源：相机（原子读，每帧）/ 业务统计（1Hz 由 Renderer 喂进 hudInfo_）。
std::size_t HudRenderer::buildHudVertices() {
    hudText_.clear();

    // ---- 相机（渲染线程写、这里只读）----
    double cx = 0.0, cy = 0.0, cz = 0.0, yaw = 0.0, pitch = 0.0;
    if (cameraState_) {
        cx    = cameraState_->camX.load(std::memory_order_relaxed);
        cy    = cameraState_->camY.load(std::memory_order_relaxed);
        cz    = cameraState_->camZ.load(std::memory_order_relaxed);
        yaw   = cameraState_->yaw.load(std::memory_order_relaxed);
        pitch = cameraState_->pitch.load(std::memory_order_relaxed);
    }

    // ---- 朝向：yaw=0 朝 -Z（north），每 45° 一个方位，顺时针排列 ----
    constexpr double kTwoPi = 6.283185307179586;
    double normYaw = std::fmod(yaw, kTwoPi);
    if (normYaw < 0.0) normYaw += kTwoPi;
    // 8 方位：四项正方向之外的斜向也标出坐标符号，风格统一（否则 "southwest" 与 "south (+Z)"
    // 混排，最长行长度随转身跳动，背板会跟着忽宽忽窄）。
    // 顺序按 yaw 递增（-Z 北方为 0），索引 = round(yaw / 45°) % 8。
    static const char* kFacing[8] = {
        "north (-Z)", "northeast (+X,-Z)", "east (+X)", "southeast (+X,+Z)",
        "south (+Z)", "southwest (-X,+Z)", "west (-X)", "northwest (-X,-Z)"};
    const int facingIndex = static_cast<int>(normYaw / (kTwoPi / 8.0) + 0.5) % 8;

    // ---- 所在 chunk + chunk 内局部坐标 ----
    // 先 floor 再位运算：bx & 15 对负数同样落在 0..15（补码），bx >> 4 是算术右移（向 -∞ 取整），
    // 与 ChunkPos.hpp 里 chunkOf 的约定一致。
    const int bx = static_cast<int>(std::floor(cx));
    const int by = static_cast<int>(std::floor(cy));
    const int bz = static_cast<int>(std::floor(cz));
    const int chunkX = bx >> 4, localX = bx & 15;
    const int chunkY = by >> 4, localY = by & 15;
    const int chunkZ = bz >> 4, localZ = bz & 15;

    // ---- 已提交的网格统计（纯内存遍历，不碰 GPU）----
    // 网格统计由 VulkanContext 每帧喂入（见 setMeshStats），本类不直接遍历网格表
    const int meshChunks = meshChunks_;
    const long long triCount = meshTriangles_;

    // ---- 文本行（英文标签：字体图集只覆盖 ASCII 32..126）----
    std::vector<std::string> lines;
    lines.reserve(10);

    // 模态覆盖层（存档菜单）：有内容就**替代**调试统计，避免两套字叠在一起
    if (!overlayLines_.empty()) {
        lines = overlayLines_;
    } else {
    char buf[200];

    std::snprintf(buf, sizeof(buf),
        "[F3] DEBUG OVERLAY     FPS %5.1f  (%.2f ms)", hudFps_, hudFrameMs_);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "XYZ      %.2f / %.2f / %.2f", cx, cy, cz);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "YAW      %6.1f   PITCH %6.1f   FACING %s",
        yaw * 57.2957795130, pitch * 57.2957795130, kFacing[facingIndex]);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "CHUNK    (%d, %d, %d)   LOCAL (%d, %d, %d)",
        chunkX, chunkY, chunkZ, localX, localY, localZ);
    lines.emplace_back(buf);

    if (hudInfo_.valid) {
        std::snprintf(buf, sizeof(buf), "CHUNKS   %lld   BLOCKS %lld",
            hudInfo_.chunkCount, hudInfo_.blockCount);
        lines.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "TICK     sim %.1f s   ticks %lld   tps %.1f",
            hudInfo_.simTime, hudInfo_.tickCount, hudInfo_.tps);
        lines.emplace_back(buf);
    } else {
        lines.emplace_back("CHUNKS   --   BLOCKS --");
        lines.emplace_back("TICK     --");
    }

    std::snprintf(buf, sizeof(buf), "MESH     chunks %d   triangles %lld",
        meshChunks, triCount);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "WINDOW   %ux%u   MSAA %dx   frames %llu",
        swapchainExtent_.width, swapchainExtent_.height,
        static_cast<int>(msaaSamples_),   // 枚举值本身即采样数（1/2/4/8/16）
        static_cast<unsigned long long>(hudFrames_));
    lines.emplace_back(buf);
    }   // ← overlay 为空时的调试统计行

    // ---- 排版：先算背板宽度 → 画板 → 画字（顶点顺序即绘制顺序，板必须先于字）----
    float maxWidth = 0.0f;
    for (const auto& line : lines) {
        const float w = hudText_.textWidth(line);
        if (w > maxWidth) maxWidth = w;   // 不用 std::max：Windows.h 的 max 宏会吃掉它
    }
    constexpr float kPanelMargin = 8.0f;  // 背板到窗口边缘
    constexpr float kInsetX = 10.0f;      // 文字到背板左缘
    constexpr float kInsetY = 8.0f;       // 文字到背板上缘
    const float panelW0 = maxWidth + kInsetX * 2.0f;
    // 背板宽度加下限：FPS 99→100、坐标位数变化会让"最长行"左右跳，背板跟着缩会看起来像在抖。
    // 只在内容真的更宽时才扩展，同时不能超出窗口（小窗口时按屏幕宽度收）。
    constexpr float kPanelMinW = 640.0f;
    float panelW = panelW0;
    if (panelW < kPanelMinW) panelW = kPanelMinW;
    const float panelMaxW = static_cast<float>(swapchainExtent_.width) - kPanelMargin * 2.0f;
    if (panelW > panelMaxW) panelW = panelMaxW;
    const float panelH = static_cast<float>(lines.size()) * hud::HudTextBuilder::kLineHeight
                       + kInsetY * 2.0f;
    hudText_.addPanel(kPanelMargin, kPanelMargin, panelW, panelH,
                      glm::vec4(0.0f, 0.0f, 0.0f, 0.55f));

    const glm::vec4 kTextColor(0.92f, 0.95f, 1.0f, 1.0f);
    const glm::vec4 kTitleColor(0.55f, 0.95f, 0.72f, 1.0f);
    float penY = kPanelMargin + kInsetY;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        hudText_.addText(kPanelMargin + kInsetX, penY, lines[i],
                         i == 0 ? kTitleColor : kTextColor);
        penY += hud::HudTextBuilder::kLineHeight;
    }

    // 容量保护：正常约 1000 顶点；超容量说明文本异常，截断而不是越界写 GPU 缓冲
    const std::size_t count = hudText_.vertexCount();
    if (count > kHudVertexCapacity) {
        ENGINE_LOG_WARN("Vulkan", "[HUD] 顶点数超容量，已截断（检查 HUD 文本长度）");
        return kHudVertexCapacity;
    }
    return count;
}

// 录制 HUD 这一趟：更新本帧顶点 → 绑定独立管线/描述符 → push 屏幕尺寸 → 一次 draw。
void HudRenderer::drawHud(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!hudPipeline_ || !hudVertexMapped_[frameIndex]) return;

    const std::size_t vcount = buildHudVertices();
    if (vcount == 0) return;

    // 首帧绘制确认：证明 HUD 真的进了命令缓冲（只打一次，不污染每帧日志）
    if (!hudDrawnOnce_) {
        hudDrawnOnce_ = true;
        std::ostringstream os;
        os << "[HUD] 首帧绘制 " << vcount << " 顶点";
        ENGINE_LOG_INFO("Vulkan", os.str());
    }

    // HOST_COHERENT 内存：memcpy 之后不需要 vkFlushMappedMemoryRanges
    std::memcpy(hudVertexMapped_[frameIndex], hudText_.vertices().data(),
                vcount * sizeof(hud::HudVertex));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout_,
                            0, 1, &hudDescSet_, 0, nullptr);
    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &hudVertexBuffer_[frameIndex], &vbOffset);

    const float screen[2] = {
        static_cast<float>(swapchainExtent_.width),
        static_cast<float>(swapchainExtent_.height)};
    vkCmdPushConstants(cmd, hudPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(screen), screen);

    vkCmdDraw(cmd, static_cast<uint32_t>(vcount), 1, 0, 0);
}

} // namespace engine::render

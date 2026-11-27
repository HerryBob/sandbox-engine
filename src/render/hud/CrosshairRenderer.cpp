#include "render/hud/CrosshairRenderer.hpp"

#include <cstring>
#include <sstream>

#include "core/Log.hpp"
#include "render/hud/HudFont.hpp"
#include "render/hud/HudTextBuilder.hpp"   // HudVertex（与 HUD 同一顶点格式）
#include "render/utils/ResourcesUtils.hpp"
#include "render/utils/ShaderUtils.hpp"

namespace engine::render::hud {

namespace {

// 追加一个轴对齐矩形（像素坐标，左上原点）到顶点数组；UV 恒取图集第 0 格（纯白）。
void pushRect(HudVertex* out, std::size_t& n,
              float x0, float y0, float x1, float y1,
              float r, float g, float b, float a) {
    const glm::vec4 uv = whiteUV();
    const glm::vec4 c(r, g, b, a);
    const glm::vec2 p[4] = { {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1} };
    const glm::vec2 t[4] = { {uv.x, uv.y}, {uv.z, uv.y}, {uv.z, uv.w}, {uv.x, uv.w} };
    const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; ++i) {
        out[n].pos = p[idx[i]];
        out[n].uv = t[idx[i]];
        out[n].color = c;
        ++n;
    }
}

} // namespace

bool CrosshairRenderer::init(VkPhysicalDevice physicalDevice, VkDevice device,
                             VkQueue graphicsQueue, VkCommandPool cmdPool,
                             VkRenderPass renderPass, VkExtent2D extent,
                             VkSampleCountFlagBits msaaSamples, const HudAtlas& inAtlas) {
    physicalDevice_ = physicalDevice;
    device_ = device;
    graphicsQueue_ = graphicsQueue;
    cmdPool_ = cmdPool;
    extent_ = extent;
    atlas_ = inAtlas;

    if (!atlas_.valid()) {
        // 没有图集就不画：宁可没有准星，也不要为它单独再上传一份贴图
        ENGINE_LOG_ERROR("Vulkan", "[准星] 未拿到 HUD 图集，准星不启用");
        return false;
    }
    if (!createDescriptor()) return false;
    if (!createPipeline(renderPass, msaaSamples)) return false;
    if (!createVertexBuffers()) return false;

    ENGINE_LOG_INFO("Vulkan", "[准星] 已启用（屏幕中心十字，共享 HUD 图集）");
    return true;
}

void CrosshairRenderer::shutdown() {
    for (std::size_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vertexMapped_[i] != nullptr) {
            vkUnmapMemory(device_, vertexMemory_[i]);
            vertexMapped_[i] = nullptr;
        }
        if (vertexBuffer_[i] != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, vertexBuffer_[i], nullptr);
            vertexBuffer_[i] = VK_NULL_HANDLE;
        }
        if (vertexMemory_[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device_, vertexMemory_[i], nullptr);
            vertexMemory_[i] = VK_NULL_HANDLE;
        }
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
        descriptorSet_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
}

bool CrosshairRenderer::createDescriptor() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[准星] 描述符布局创建失败");
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
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[准星] 描述符池创建失败");
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descriptorSetLayout_;
    if (vkAllocateDescriptorSets(device_, &allocInfo, &descriptorSet_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[准星] 描述符集分配失败");
        return false;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = atlas_.view;
    imageInfo.sampler = atlas_.sampler;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return true;
}

bool CrosshairRenderer::createPipeline(VkRenderPass renderPass, VkSampleCountFlagBits msaaSamples) {
    // 与 HUD 共用同一对着色器：2D 像素坐标 + 图集覆盖率采样，语义完全一致
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

    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = sizeof(HudVertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = static_cast<uint32_t>(offsetof(HudVertex, pos));
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = static_cast<uint32_t>(offsetof(HudVertex, uv));
    attrs[2].location = 2;
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[2].offset = static_cast<uint32_t>(offsetof(HudVertex, color));

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bind;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

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
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = msaaSamples;   // 必须与 renderPass 一致

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

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(float) * 2;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[准星] 管线布局创建失败");
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
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                 nullptr, &pipeline_);
    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "[准星] vkCreateGraphicsPipelines 失败 result=" << r;
        ENGINE_LOG_ERROR("Vulkan", os.str());
        return false;
    }
    return true;
}

bool CrosshairRenderer::createVertexBuffers() {
    const VkDeviceSize size = sizeof(HudVertex) * kVertexCapacity;
    for (std::size_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (!util::createBuffer(device_, physicalDevice_, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                vertexBuffer_[i], vertexMemory_[i])) {
            ENGINE_LOG_ERROR("Vulkan", "[准星] 顶点缓冲创建失败");
            return false;
        }
        if (vkMapMemory(device_, vertexMemory_[i], 0, size, 0, &vertexMapped_[i]) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("Vulkan", "[准星] 顶点缓冲映射失败");
            return false;
        }
    }
    return true;
}

void CrosshairRenderer::draw(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!visible_ || pipeline_ == VK_NULL_HANDLE || extent_.width == 0 || extent_.height == 0) return;

    // 经典十字：中心留空，四段短条。全部用整数像素，避免在高 DPI 下糊边。
    const float cx = static_cast<float>(extent_.width) * 0.5f;
    const float cy = static_cast<float>(extent_.height) * 0.5f;
    const float halfT = static_cast<float>(thickness_) * 0.5f;
    const float gap = static_cast<float>(gap_) * 0.5f;      // 中心留空的一半
    const float arm = static_cast<float>(armLen_);

    HudVertex* v = static_cast<HudVertex*>(vertexMapped_[frameIndex]);
    if (v == nullptr) return;
    std::size_t n = 0;

    // 先画描边：同样的十字，四周各外扩 1px、中心留空也外扩 1px，压成半透明暗色。
    // 目的：白准星压在草地/雪地这种高亮背景上仍然可辨（不改颜色也能看清）。
    {
        const float ot = halfT + 1.0f;
        const float og = gap - 1.0f;      // 负值也无妨：中心会被主体盖住一部分
        const float oa = arm + 1.0f;
        const float r = 0.0f, g = 0.0f, b = 0.0f, a = outlineA_;
        pushRect(v, n, cx - og - oa, cy - ot, cx - og, cy + ot, r, g, b, a);
        pushRect(v, n, cx + og, cy - ot, cx + og + oa, cy + ot, r, g, b, a);
        pushRect(v, n, cx - ot, cy - og - oa, cx + ot, cy - og, r, g, b, a);
        pushRect(v, n, cx - ot, cy + og, cx + ot, cy + og + oa, r, g, b, a);
    }
    // 再画主体
    pushRect(v, n, cx - gap - arm, cy - halfT, cx - gap, cy + halfT, colorR_, colorG_, colorB_, colorA_);
    pushRect(v, n, cx + gap,       cy - halfT, cx + gap + arm, cy + halfT, colorR_, colorG_, colorB_, colorA_);
    pushRect(v, n, cx - halfT, cy - gap - arm, cx + halfT, cy - gap, colorR_, colorG_, colorB_, colorA_);
    pushRect(v, n, cx - halfT, cy + gap,       cx + halfT, cy + gap + arm, colorR_, colorG_, colorB_, colorA_);

    const float screen[2] = { static_cast<float>(extent_.width), static_cast<float>(extent_.height) };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &descriptorSet_, 0, nullptr);
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(screen), screen);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_[frameIndex], &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(n), 1, 0, 0);
}

} // namespace engine::render::hud

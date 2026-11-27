#pragma once
#include <cstdint>

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

namespace engine::render::hud {

// ============================================================================
// HudAtlas：F3 浮层用的位图字体图集句柄（**只读共享**）
// ============================================================================
// 由 HudRenderer 创建并持有；其它 2D 元素（准星、血条、图标）借用同一份纹理，
// 避免每加一个 HUD 元素就多上传一次同一张图集。
//
// 为什么单独抽成 POD：这些 2D 元素将来要以 DLL 模组形式提供，
// 模组只需要"宿主给的图集 + 设备/渲染通道"就能自己开管线，不需要知道 HudRenderer 内部。
struct HudAtlas {
    VkImage     image   = VK_NULL_HANDLE;
    VkImageView view    = VK_NULL_HANDLE;
    VkSampler   sampler = VK_NULL_HANDLE;
    bool valid() const { return view != VK_NULL_HANDLE && sampler != VK_NULL_HANDLE; }
};

} // namespace engine::render::hud

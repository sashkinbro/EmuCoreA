#pragma once
#include "Common/GPU/Vulkan/VulkanLoader.h"
#include <cstdint>
#include <vector>
template <typename T> T TestHandle(uintptr_t value) { return reinterpret_cast<T>(value); }
class VulkanContext {
public:
    VkDevice GetDevice() const { return TestHandle<VkDevice>(1); }
    VkSwapchainKHR GetSwapchain() const { return swapchain; }
    int GetGraphicsQueueFamilyIndex() const { return 0; }
    VkQueue GetGraphicsQueue() const { return TestHandle<VkQueue>(3); }
    VkInstance GetInstance() const { return TestHandle<VkInstance>(4); }
    VkPhysicalDevice GetCurrentPhysicalDevice() const { return TestHandle<VkPhysicalDevice>(5); }
    const VkSurfaceCapabilitiesKHR &GetSurfaceCapabilities() const { return caps; }
    const std::vector<VkSurfaceFormatKHR> &SurfaceFormats() const { return formats; }
    bool MemoryTypeFromProperties(uint32_t, VkFlags, uint32_t *index) { *index = 0; return true; }
    void DestroySwapchain() { swapchainDestroyed = true; }
    void DestroySurface() { surfaceDestroyed = true; }
    void WaitUntilQueueIdle() { PPSSPP_VK::vkQueueWaitIdle(GetGraphicsQueue()); }
    bool swapchainDestroyed = false;
    bool surfaceDestroyed = false;
    VkSwapchainKHR swapchain = TestHandle<VkSwapchainKHR>(2);
    VkSurfaceCapabilitiesKHR caps = [] {
        VkSurfaceCapabilitiesKHR result{};
        result.currentExtent = {8, 8};
        result.minImageExtent = {1, 1};
        result.maxImageExtent = {1024, 1024};
        result.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        result.supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        return result;
    }();
    std::vector<VkSurfaceFormatKHR> formats{{VK_FORMAT_R8G8B8A8_UNORM, VK_COLORSPACE_SRGB_NONLINEAR_KHR}};
};

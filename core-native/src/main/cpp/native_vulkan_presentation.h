// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary

#pragma once

#include "Common/GPU/Vulkan/VulkanPresentation.h"

#include <cstdint>
#include <string>
#include <vector>

class VulkanContext;

// Presentation backend for the native PPSSPP path.
//
// The default PPSSPP Vulkan path renders the finished frame straight into the
// real Android swapchain image and then calls vkQueuePresentKHR. This backend
// keeps that exact rendering path, but intercepts the final hand-off:
// QueuePresent() first copies the finished swapchain image into a private
// readback image, runs the librashader filter chain from that copy back into
// the same swapchain image, and only then presents. The swapchain keeps its
// normal format/layout handling because PPSSPP keeps rendering into the real
// images.
//
// When the shader chain is disabled, not built in, or fails to load, the
// backend simply forwards AcquireNextImage/QueuePresent to the real swapchain,
// so it is also a safe permanent pass-through.
class NativeVulkanPresentation : public VulkanPresentation {
public:
    explicit NativeVulkanPresentation(VulkanContext *vulkan);
    ~NativeVulkanPresentation() override;

    // Must be called before SetPresentation(). Returns false if the backend
    // cannot allocate its device objects; the caller should then keep the
    // default real-swapchain behavior.
    bool Create(VulkanContext *vulkan);

    void Destroy(VulkanContext *vulkan) override;
    bool NeedsRecreate() const override { return false; }
    bool Recreate(VulkanContext *vulkan) override { (void)vulkan; return true; }

    VkResult AcquireNextImage(VulkanContext *vulkan, VkSemaphore signalSemaphore, uint32_t *imageIndex) override;
    VkResult QueuePresent(VulkanContext *vulkan, VkQueue queue, uint32_t imageIndex, VkSemaphore waitSemaphore) override;

    uint32_t GetImageCount() const override;
    VkImage GetImage(uint32_t index) const override;
    VkExtent2D GetExtent() const override;
    VkFormat GetFormat() const override;
    VkImageLayout GetPresentLayout() const override { return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; }

private:
    VkResult PresentDirect(VulkanContext *vulkan, VkQueue queue, uint32_t imageIndex, VkSemaphore waitSemaphore);
    VkExtent2D QueryExtent() const;
    VkFormat QueryFormat() const;
    void RefreshImages() const;
    bool EnsurePresentResources(VulkanContext *vulkan, uint32_t imageIndex);

    bool EnsureChain(VulkanContext *vulkan);
    void DestroyChain();
    bool EnsureInputImage(VulkanContext *vulkan, VkExtent2D extent, VkFormat format);
    void DestroyInputImage(VulkanContext *vulkan);
    bool RecordChainPass(VulkanContext *vulkan, VkCommandBuffer cmd, VkImage swapchainImage,
                         VkExtent2D extent, VkFormat format);

    VulkanContext *vulkan_ = nullptr;

    mutable VkSwapchainKHR cachedSwapchain_ = VK_NULL_HANDLE;
    mutable std::vector<VkImage> cachedImages_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    std::vector<VkSemaphore> presentSemaphores_;

#if defined(EMUCOREA_HAVE_LIBRASHADER)
    void *chain_ = nullptr;
    std::string chainPreset_;
    uint64_t chainGeneration_ = 0;
    bool chainFailed_ = false;
    uint64_t chainFrameCount_ = 0;

    VkImage inputImage_ = VK_NULL_HANDLE;
    VkDeviceMemory inputMemory_ = VK_NULL_HANDLE;
    uint32_t inputWidth_ = 0;
    uint32_t inputHeight_ = 0;
    VkFormat inputFormat_ = VK_FORMAT_UNDEFINED;
#endif
};

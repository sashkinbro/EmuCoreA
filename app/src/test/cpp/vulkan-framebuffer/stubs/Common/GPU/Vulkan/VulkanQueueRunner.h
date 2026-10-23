#pragma once
#include "Common/GPU/Vulkan/VulkanFramebuffer.h"
#include <vector>
class VulkanBarrierBatch {
public:
    std::vector<VkImageMemoryBarrier> images;
    VkImageMemoryBarrier *Add(VkImage image, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags) {
        images.push_back({}); images.back().image = image; return &images.back();
    }
    void TransitionColorImageAuto(VKRImage *image, VkImageLayout layout) {
        Add(image->image, 0, 0, 0); image->layout = layout;
    }
    void TransitionDepthStencilImageAuto(VKRImage *image, VkImageLayout layout) {
        Add(image->image, 0, 0, 0); image->layout = layout;
    }
};

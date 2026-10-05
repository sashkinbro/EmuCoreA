// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary

#include "native_vulkan_presentation.h"

#include <android/log.h>

#include <algorithm>
#include <cstdint>

#include "Common/GPU/Vulkan/VulkanContext.h"
#include "shader_chain.h"

#if defined(EMUCOREA_HAVE_LIBRASHADER)
#include <librashader.h>
#endif

using namespace PPSSPP_VK;

#define NVK_LOG_TAG "EmuCoreA-Shader"
#define NVK_LOGI(...) __android_log_print(ANDROID_LOG_INFO, NVK_LOG_TAG, __VA_ARGS__)
#define NVK_LOGW(...) __android_log_print(ANDROID_LOG_WARN, NVK_LOG_TAG, __VA_ARGS__)
#define NVK_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, NVK_LOG_TAG, __VA_ARGS__)

namespace {

template <typename T>
T Clamp(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

const VkImageSubresourceRange kColorRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

}  // namespace

NativeVulkanPresentation::NativeVulkanPresentation(VulkanContext *vulkan) : vulkan_(vulkan) {
}

NativeVulkanPresentation::~NativeVulkanPresentation() = default;

bool NativeVulkanPresentation::Create(VulkanContext *vulkan) {
    if (vulkan == nullptr || vulkan->GetDevice() == VK_NULL_HANDLE || vulkan->GetSwapchain() == VK_NULL_HANDLE) {
        return false;
    }
    vulkan_ = vulkan;

    VkCommandPoolCreateInfo info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = (uint32_t)vulkan->GetGraphicsQueueFamilyIndex();
    if (vkCreateCommandPool(vulkan->GetDevice(), &info, nullptr, &commandPool_) != VK_SUCCESS) {
        NVK_LOGE("vkCreateCommandPool failed");
        commandPool_ = VK_NULL_HANDLE;
        return false;
    }
    RefreshImages();
    return true;
}

void NativeVulkanPresentation::Destroy(VulkanContext *vulkan) {
    if (vulkan == nullptr) return;

#if defined(EMUCOREA_HAVE_LIBRASHADER)
    DestroyChain();
#endif
    DestroyInputImage(vulkan);

    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vulkan->GetDevice(), commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }
    commandBuffers_.clear();
    for (VkSemaphore &semaphore : presentSemaphores_) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vulkan->GetDevice(), semaphore, nullptr);
        }
        semaphore = VK_NULL_HANDLE;
    }
    presentSemaphores_.clear();
    cachedImages_.clear();
    cachedSwapchain_ = VK_NULL_HANDLE;

    // VulkanGraphicsContext::ShutdownSurface() skips its swapchain/surface teardown when a
    // presentation is installed, so mirror it here. Otherwise the next InitSurface()
    // would destroy the surface while the old swapchain still references it.
    vulkan->DestroySwapchain();
    vulkan->DestroySurface();
    vulkan_ = nullptr;
}

VkResult NativeVulkanPresentation::AcquireNextImage(VulkanContext *vulkan, VkSemaphore signalSemaphore,
                                                    uint32_t *imageIndex) {
    if (vulkan == nullptr || imageIndex == nullptr) return VK_ERROR_SURFACE_LOST_KHR;
    const VkSwapchainKHR swapchain = vulkan->GetSwapchain();
    if (swapchain == VK_NULL_HANDLE) return VK_ERROR_SURFACE_LOST_KHR;
    return vkAcquireNextImageKHR(vulkan->GetDevice(), swapchain, UINT64_MAX, signalSemaphore, VK_NULL_HANDLE,
                                 imageIndex);
}

VkResult NativeVulkanPresentation::PresentDirect(VulkanContext *vulkan, VkQueue queue, uint32_t imageIndex,
                                                 VkSemaphore waitSemaphore) {
    const VkSwapchainKHR swapchain = vulkan->GetSwapchain();
    if (swapchain == VK_NULL_HANDLE) return VK_ERROR_SURFACE_LOST_KHR;

    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    if (waitSemaphore != VK_NULL_HANDLE) {
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &waitSemaphore;
    }
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    return vkQueuePresentKHR(queue, &present);
}

VkResult NativeVulkanPresentation::QueuePresent(VulkanContext *vulkan, VkQueue queue, uint32_t imageIndex,
                                                VkSemaphore waitSemaphore) {
    if (vulkan == nullptr || vulkan->GetSwapchain() == VK_NULL_HANDLE) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }

#if defined(EMUCOREA_HAVE_LIBRASHADER)
    // Reading the finished frame back requires the swapchain image to accept
    // TRANSFER_SRC. Surfaces that do not expose it simply stay on the
    // pass-through path.
    const bool canCopyFromSwapchain =
        (vulkan->GetSurfaceCapabilities().supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (canCopyFromSwapchain && emucorer::shader_chain::IsEnabled() &&
        EnsurePresentResources(vulkan, imageIndex) && EnsureChain(vulkan)) {
        const VkExtent2D extent = QueryExtent();
        const VkFormat format = QueryFormat();
        const VkImage swapchainImage = GetImage(imageIndex);
        if (swapchainImage != VK_NULL_HANDLE && EnsureInputImage(vulkan, extent, format) &&
            RecordChainPass(vulkan, commandBuffers_[imageIndex], swapchainImage, extent, format)) {
            const VkCommandBuffer cmd = commandBuffers_[imageIndex];
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &waitSemaphore;
            submit.pWaitDstStageMask = &waitStage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &presentSemaphores_[imageIndex];

            const VkResult submitResult = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
            if (submitResult == VK_SUCCESS) {
                const VkSwapchainKHR swapchain = vulkan->GetSwapchain();
                VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
                present.waitSemaphoreCount = 1;
                present.pWaitSemaphores = &presentSemaphores_[imageIndex];
                present.swapchainCount = 1;
                present.pSwapchains = &swapchain;
                present.pImageIndices = &imageIndex;
                return vkQueuePresentKHR(queue, &present);
            }
            if (submitResult == VK_ERROR_DEVICE_LOST) {
                NVK_LOGE("Device lost while submitting the shader chain");
                return submitResult;
            }
            NVK_LOGE("vkQueueSubmit (shader chain) failed: %d", (int)submitResult);
        }
    }
#endif

    return PresentDirect(vulkan, queue, imageIndex, waitSemaphore);
}

VkExtent2D NativeVulkanPresentation::QueryExtent() const {
    const VkSurfaceCapabilitiesKHR &caps = vulkan_->GetSurfaceCapabilities();
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
        extent = caps.minImageExtent;
    }
    extent.width = Clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = Clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);

    // Mirror VulkanContext::InitSwapchain(): a 90-degree pre-rotation swaps the
    // swapchain extent, and the backbuffer sizes must follow it.
    const VkSurfaceTransformFlagsKHR t = caps.currentTransform;
    const bool identity = (t & (VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR)) != 0;
    const bool allowed = (t & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR)) != 0;
    if (!identity && allowed && (t & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR))) {
        std::swap(extent.width, extent.height);
    }
    return extent;
}

VkFormat NativeVulkanPresentation::QueryFormat() const {
    // Mirror VulkanContext::InitSwapchain()'s format selection so the views and
    // render passes the render manager builds from us match the real swapchain.
    const std::vector<VkSurfaceFormatKHR> &formats = vulkan_->SurfaceFormats();
    if (formats.empty() || (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED)) {
        return VK_FORMAT_B8G8R8A8_UNORM;
    }
    for (const VkSurfaceFormatKHR &format : formats) {
        if (format.colorSpace != VK_COLORSPACE_SRGB_NONLINEAR_KHR) continue;
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) {
            return format.format;
        }
    }
    return formats[0].format;
}

void NativeVulkanPresentation::RefreshImages() const {
    const VkSwapchainKHR swapchain = vulkan_->GetSwapchain();
    if (swapchain == cachedSwapchain_ && !cachedImages_.empty()) return;

    cachedImages_.clear();
    cachedSwapchain_ = swapchain;
    if (swapchain == VK_NULL_HANDLE) return;

    const VkDevice device = vulkan_->GetDevice();
    uint32_t count = 0;
    VkResult result = vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) return;
    for (int attempt = 0; attempt < 3; ++attempt) {
        cachedImages_.resize(count);
        result = vkGetSwapchainImagesKHR(device, swapchain, &count, cachedImages_.data());
        if (result == VK_SUCCESS) {
            cachedImages_.resize(count);
            return;
        }
        if (result != VK_INCOMPLETE) break;
    }
    NVK_LOGW("vkGetSwapchainImagesKHR failed (0x%x)", (unsigned)result);
    cachedImages_.clear();
}

uint32_t NativeVulkanPresentation::GetImageCount() const {
    RefreshImages();
    return (uint32_t)cachedImages_.size();
}

VkImage NativeVulkanPresentation::GetImage(uint32_t index) const {
    RefreshImages();
    if (index >= cachedImages_.size()) return VK_NULL_HANDLE;
    return cachedImages_[index];
}

VkExtent2D NativeVulkanPresentation::GetExtent() const {
    return QueryExtent();
}

VkFormat NativeVulkanPresentation::GetFormat() const {
    return QueryFormat();
}

bool NativeVulkanPresentation::EnsurePresentResources(VulkanContext *vulkan, uint32_t imageIndex) {
    if (commandPool_ == VK_NULL_HANDLE) return false;

    const size_t newSize = imageIndex + 1;
    if (presentSemaphores_.size() < newSize) {
        presentSemaphores_.resize(newSize, VK_NULL_HANDLE);
        commandBuffers_.resize(newSize, VK_NULL_HANDLE);
    }

    for (size_t i = 0; i < newSize; ++i) {
        if (presentSemaphores_[i] == VK_NULL_HANDLE) {
            VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (vkCreateSemaphore(vulkan->GetDevice(), &semaphoreInfo, nullptr, &presentSemaphores_[i]) !=
                VK_SUCCESS) {
                NVK_LOGE("vkCreateSemaphore (present) failed");
                presentSemaphores_[i] = VK_NULL_HANDLE;
                return false;
            }
        }
        if (commandBuffers_[i] == VK_NULL_HANDLE) {
            VkCommandBufferAllocateInfo alloc{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            alloc.commandPool = commandPool_;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(vulkan->GetDevice(), &alloc, &commandBuffers_[i]) != VK_SUCCESS) {
                NVK_LOGE("vkAllocateCommandBuffers (present) failed");
                commandBuffers_[i] = VK_NULL_HANDLE;
                return false;
            }
        }
    }
    return true;
}

#if defined(EMUCOREA_HAVE_LIBRASHADER)

namespace {

void ReportChainError(const char *what, libra_error_t error) {
    char *message = nullptr;
    if (libra_error_write(error, &message) == 0 && message != nullptr) {
        NVK_LOGE("librashader %s failed: %s", what, message);
        libra_error_free_string(&message);
    } else {
        NVK_LOGE("librashader %s failed (errno %d)", what, (int)libra_error_errno(error));
    }
    libra_error_free(&error);
}

}  // namespace

bool NativeVulkanPresentation::EnsureChain(VulkanContext *vulkan) {
    const std::string path = emucorer::shader_chain::PresetPath();
    if (path.empty() || !emucorer::shader_chain::IsEnabled()) return false;

    const uint64_t generation = emucorer::shader_chain::Generation();
    if (chain_ != nullptr && chainPreset_ == path && chainGeneration_ == generation) return true;
    if (chainFailed_ && chainPreset_ == path && chainGeneration_ == generation) return false;

    DestroyChain();
    chainPreset_ = path;
    chainGeneration_ = generation;

    libra_shader_preset_t preset = nullptr;
    if (libra_error_t error = libra_preset_create(path.c_str(), &preset)) {
        ReportChainError("preset load", error);
        chainFailed_ = true;
        return false;
    }

    libra_device_vk_t device{};
    device.physical_device = vulkan->GetCurrentPhysicalDevice();
    device.instance = vulkan->GetInstance();
    device.device = vulkan->GetDevice();
    device.queue = vulkan->GetGraphicsQueue();
    device.entry = vkGetInstanceProcAddr;

    libra_vk_filter_chain_t chain = nullptr;
    if (libra_error_t error = libra_vk_filter_chain_create(&preset, device, nullptr, &chain)) {
        ReportChainError("chain create", error);
        chainFailed_ = true;
        return false;
    }
    chain_ = chain;
    chainFrameCount_ = 0;
    NVK_LOGI("librashader chain loaded: %s", path.c_str());
    return true;
}

void NativeVulkanPresentation::DestroyChain() {
    if (chain_ != nullptr) {
        libra_vk_filter_chain_t chain = static_cast<libra_vk_filter_chain_t>(chain_);
        libra_vk_filter_chain_free(&chain);
        chain_ = nullptr;
    }
    chainPreset_.clear();
    chainGeneration_ = 0;
    chainFailed_ = false;
    chainFrameCount_ = 0;
}

bool NativeVulkanPresentation::EnsureInputImage(VulkanContext *vulkan, VkExtent2D extent, VkFormat format) {
    if (extent.width == 0 || extent.height == 0) return false;
    if (inputImage_ != VK_NULL_HANDLE && inputWidth_ == extent.width && inputHeight_ == extent.height &&
        inputFormat_ == format) {
        return true;
    }
    DestroyInputImage(vulkan);

    VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = { extent.width, extent.height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vulkan->GetDevice(), &imageInfo, nullptr, &inputImage_) != VK_SUCCESS) {
        NVK_LOGE("vkCreateImage (shader chain input) failed");
        inputImage_ = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(vulkan->GetDevice(), inputImage_, &requirements);
    uint32_t memoryType = 0;
    if (!vulkan->MemoryTypeFromProperties(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          &memoryType)) {
        NVK_LOGE("No device-local memory for the shader chain input");
        DestroyInputImage(vulkan);
        return false;
    }

    VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(vulkan->GetDevice(), &allocateInfo, nullptr, &inputMemory_) != VK_SUCCESS) {
        NVK_LOGE("vkAllocateMemory (shader chain input) failed");
        DestroyInputImage(vulkan);
        return false;
    }
    if (vkBindImageMemory(vulkan->GetDevice(), inputImage_, inputMemory_, 0) != VK_SUCCESS) {
        NVK_LOGE("vkBindImageMemory (shader chain input) failed");
        DestroyInputImage(vulkan);
        return false;
    }

    inputWidth_ = extent.width;
    inputHeight_ = extent.height;
    inputFormat_ = format;
    return true;
}

void NativeVulkanPresentation::DestroyInputImage(VulkanContext *vulkan) {
    if (vulkan == nullptr || vulkan->GetDevice() == VK_NULL_HANDLE) return;
    if (inputImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(vulkan->GetDevice(), inputImage_, nullptr);
        inputImage_ = VK_NULL_HANDLE;
    }
    if (inputMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(vulkan->GetDevice(), inputMemory_, nullptr);
        inputMemory_ = VK_NULL_HANDLE;
    }
    inputWidth_ = 0;
    inputHeight_ = 0;
    inputFormat_ = VK_FORMAT_UNDEFINED;
}

bool NativeVulkanPresentation::RecordChainPass(VulkanContext *vulkan, VkCommandBuffer cmd, VkImage swapchainImage,
                                               VkExtent2D extent, VkFormat format) {
    if (chain_ == nullptr || inputImage_ == VK_NULL_HANDLE) return false;

    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return false;

    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = kColorRange;

    // The frame's backbuffer render pass left the swapchain image in
    // PRESENT_SRC; make it readable so it can be copied out.
    barrier.image = swapchainImage;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    // The input copy is fully overwritten every frame, so discard its contents.
    barrier.image = inputImage_;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    VkImageCopy copy{};
    copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.extent = { extent.width, extent.height, 1 };
    vkCmdCopyImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, inputImage_,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    // librashader samples its input from SHADER_READ_ONLY_OPTIMAL.
    barrier.image = inputImage_;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    const libra_image_vk_t in{ inputImage_, inputFormat_, inputWidth_, inputHeight_ };
    const libra_image_vk_t out{ swapchainImage, format, extent.width, extent.height };
    const libra_viewport_t viewport{ 0.0f, 0.0f, extent.width, extent.height };

    libra_vk_filter_chain_t chain = static_cast<libra_vk_filter_chain_t>(chain_);
    if (libra_error_t error = libra_vk_filter_chain_frame(&chain, cmd, (size_t)chainFrameCount_, in, out, &viewport,
                                                          nullptr, nullptr)) {
        ReportChainError("frame", error);
        // Nothing has executed yet; drop the recorded work and present the
        // untouched frame through the pass-through path instead.
        chainFailed_ = true;
        vkEndCommandBuffer(cmd);
        return false;
    }
    ++chainFrameCount_;

    // The final librashader pass leaves the output in COLOR_ATTACHMENT_OPTIMAL.
    barrier.image = swapchainImage;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    return vkEndCommandBuffer(cmd) == VK_SUCCESS;
}

#else  // !EMUCOREA_HAVE_LIBRASHADER

bool NativeVulkanPresentation::EnsureChain(VulkanContext *) { return false; }
void NativeVulkanPresentation::DestroyChain() {}
bool NativeVulkanPresentation::EnsureInputImage(VulkanContext *, VkExtent2D, VkFormat) { return false; }
void NativeVulkanPresentation::DestroyInputImage(VulkanContext *) {}
bool NativeVulkanPresentation::RecordChainPass(VulkanContext *, VkCommandBuffer, VkImage, VkExtent2D, VkFormat) {
    return false;
}

#endif  // EMUCOREA_HAVE_LIBRASHADER

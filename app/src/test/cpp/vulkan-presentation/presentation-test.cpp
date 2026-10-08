// Executes the production presentation class against a contract-checking driver.
// The fake GPU keeps submissions pending until a fence/queue wait, making host
// resource lifetime mistakes deterministic without a particular device/driver.
#include "native_vulkan_presentation.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "shader_chain.h"
#include <librashader.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
int failures = 0;
uintptr_t nextHandle = 100;
bool failChainFrame = false;
bool failSubmit = false;
int chainFrameCalls = 0;
int queueWaits = 0;
int fenceWaits = 0;
uint32_t swapchainImageCount = 2;
std::vector<std::string> errors;
std::set<VkCommandBuffer> pendingCommands;
std::set<VkImage> pendingImages;
std::set<libra_vk_filter_chain_t> pendingChains;
std::map<VkFence, bool> fences;
std::map<VkFence, VkCommandBuffer> fenceCommands;
std::map<VkCommandBuffer, libra_vk_filter_chain_t> commandChains;
std::map<VkCommandBuffer, std::set<VkImage>> commandImages;
std::map<VkCommandBuffer, std::map<VkImage, VkImageLayout>> layouts;
std::map<libra_vk_filter_chain_t, std::vector<VkCommandBuffer>> chainSlots;

template<typename T> T NewHandle() { return TestHandle<T>(nextHandle++); }
void Check(bool condition, const char *message) {
    if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}
void Complete() {
    pendingCommands.clear(); pendingImages.clear(); pendingChains.clear();
    for (auto &fence : fences) fence.second = true;
}
void Reset() {
    errors.clear(); Complete(); layouts.clear(); commandChains.clear(); commandImages.clear();
    fenceCommands.clear(); chainFrameCalls = queueWaits = fenceWaits = 0;
    chainSlots.clear(); swapchainImageCount = 2;
    failChainFrame = failSubmit = false;
    emucorer::shader_chain::SetPreset("identity.slangp", true);
}
void CheckDriver(const char *message) {
    Check(errors.empty(), message);
    for (const auto &error : errors) std::printf("  driver: %s\n", error.c_str());
}
}

namespace PPSSPP_VK {
PFN_vkCreateCommandPool vkCreateCommandPool = [](VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *out) { *out = NewHandle<VkCommandPool>(); return VK_SUCCESS; };
PFN_vkDestroyCommandPool vkDestroyCommandPool = [](VkDevice, VkCommandPool, const VkAllocationCallbacks *) {};
PFN_vkDestroySemaphore vkDestroySemaphore = [](VkDevice, VkSemaphore, const VkAllocationCallbacks *) {};
PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = [](VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *out) { *out = 0; return VK_SUCCESS; };
PFN_vkQueuePresentKHR vkQueuePresentKHR = [](VkQueue, const VkPresentInfoKHR *) { return VK_SUCCESS; };
PFN_vkQueueSubmit vkQueueSubmit = [](VkQueue, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    if (failSubmit) { failSubmit = false; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    for (uint32_t i = 0; i < count; ++i) for (uint32_t j = 0; j < info[i].commandBufferCount; ++j) {
        VkCommandBuffer cmd = info[i].pCommandBuffers[j];
        pendingCommands.insert(cmd);
        pendingChains.insert(commandChains[cmd]);
        pendingImages.insert(commandImages[cmd].begin(), commandImages[cmd].end());
        if (fence) { fences[fence] = false; fenceCommands[fence] = cmd; }
    }
    return VK_SUCCESS;
};
PFN_vkQueueWaitIdle vkQueueWaitIdle = [](VkQueue) { ++queueWaits; Complete(); return VK_SUCCESS; };
PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = [](VkDevice, VkSwapchainKHR, uint32_t *count, VkImage *out) { *count = swapchainImageCount; if (out) for (uint32_t i = 0; i < *count; ++i) out[i] = TestHandle<VkImage>(10 + i); return VK_SUCCESS; };
PFN_vkCreateSemaphore vkCreateSemaphore = [](VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *, VkSemaphore *out) { *out = NewHandle<VkSemaphore>(); return VK_SUCCESS; };
PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = [](VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *out) { *out = NewHandle<VkCommandBuffer>(); return VK_SUCCESS; };
PFN_vkCreateImage vkCreateImage = [](VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *out) { *out = NewHandle<VkImage>(); return VK_SUCCESS; };
PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = [](VkDevice, VkImage, VkMemoryRequirements *out) { *out = {1024, 16, 1}; };
PFN_vkAllocateMemory vkAllocateMemory = [](VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *out) { *out = NewHandle<VkDeviceMemory>(); return VK_SUCCESS; };
PFN_vkBindImageMemory vkBindImageMemory = [](VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; };
PFN_vkDestroyImage vkDestroyImage = [](VkDevice, VkImage image, const VkAllocationCallbacks *) { if (pendingImages.count(image)) errors.push_back("destroyed image used by a pending submission"); };
PFN_vkFreeMemory vkFreeMemory = [](VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) {};
PFN_vkResetCommandBuffer vkResetCommandBuffer = [](VkCommandBuffer cmd, VkCommandBufferResetFlags) { if (pendingCommands.count(cmd)) errors.push_back("reset a pending command buffer"); layouts[cmd].clear(); commandImages[cmd].clear(); return VK_SUCCESS; };
PFN_vkBeginCommandBuffer vkBeginCommandBuffer = [](VkCommandBuffer, const VkCommandBufferBeginInfo *) { return VK_SUCCESS; };
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = [](VkCommandBuffer cmd, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *, uint32_t count, const VkImageMemoryBarrier *barriers) {
    for (uint32_t i = 0; i < count; ++i) { layouts[cmd][barriers[i].image] = barriers[i].newLayout; commandImages[cmd].insert(barriers[i].image); }
};
PFN_vkCmdCopyImage vkCmdCopyImage = [](VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy *) {};
PFN_vkEndCommandBuffer vkEndCommandBuffer = [](VkCommandBuffer) { return VK_SUCCESS; };
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = [](VkInstance, const char *) -> PFN_vkVoidFunction { return nullptr; };
PFN_vkCreateFence vkCreateFence = [](VkDevice, const VkFenceCreateInfo *info, const VkAllocationCallbacks *, VkFence *out) { *out = NewHandle<VkFence>(); fences[*out] = (info->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0; return VK_SUCCESS; };
PFN_vkDestroyFence vkDestroyFence = [](VkDevice, VkFence fence, const VkAllocationCallbacks *) { fences.erase(fence); fenceCommands.erase(fence); };
PFN_vkWaitForFences vkWaitForFences = [](VkDevice, uint32_t count, const VkFence *waits, VkBool32, uint64_t) {
    ++fenceWaits;
    for (uint32_t i = 0; i < count; ++i) {
        if (!fences[waits[i]] && !fenceCommands.count(waits[i])) { errors.push_back("waited on fence with no submitted work"); return VK_TIMEOUT; }
        if (fenceCommands.count(waits[i])) { pendingCommands.erase(fenceCommands[waits[i]]); fences[waits[i]] = true; }
    }
    return VK_SUCCESS;
};
PFN_vkResetFences vkResetFences = [](VkDevice, uint32_t count, const VkFence *reset) { for (uint32_t i = 0; i < count; ++i) { fences[reset[i]] = false; fenceCommands.erase(reset[i]); } return VK_SUCCESS; };
}

extern "C" {
libra_error_t libra_preset_create(const char *, libra_shader_preset_t *out) { *out = NewHandle<libra_shader_preset_t>(); return nullptr; }
libra_error_t libra_vk_filter_chain_create(libra_shader_preset_t *preset, libra_device_vk_t, const filter_chain_vk_opt_t *options, libra_vk_filter_chain_t *out) { *preset = nullptr; *out = NewHandle<libra_vk_filter_chain_t>(); chainSlots[*out].resize(options && options->frames_in_flight ? options->frames_in_flight : 3); Complete(); return nullptr; }
libra_error_t libra_vk_filter_chain_free(libra_vk_filter_chain_t *chain) { if (pendingChains.count(*chain)) errors.push_back("freed chain used by a pending submission"); *chain = nullptr; return nullptr; }
libra_error_t libra_vk_filter_chain_frame(libra_vk_filter_chain_t *chain, VkCommandBuffer cmd, size_t frame, libra_image_vk_t input, libra_image_vk_t output, const libra_viewport_t *, const float *, const frame_vk_opt_t *) {
    ++chainFrameCalls;
    if (layouts[cmd][input.handle] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) errors.push_back("shader input is not in SHADER_READ_ONLY_OPTIMAL");
    if (layouts[cmd][output.handle] != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) errors.push_back("shader output is not in COLOR_ATTACHMENT_OPTIMAL");
    commandChains[cmd] = *chain;
    auto &slots = chainSlots[*chain];
    auto &slot = slots[frame % slots.size()];
    if (pendingCommands.count(slot)) errors.push_back("disposed shader frame resources still used by a pending submission");
    slot = cmd;
    return failChainFrame ? NewHandle<libra_error_t>() : nullptr;
}
int32_t libra_error_write(libra_error_t, char **) { return 1; }
LIBRA_ERRNO libra_error_errno(libra_error_t) { return static_cast<LIBRA_ERRNO>(1); }
int32_t libra_error_free(libra_error_t *error) { *error = nullptr; return 0; }
int32_t libra_error_free_string(char **) { return 0; }
}

int main() {
    const VkSemaphore finished = TestHandle<VkSemaphore>(50);
#if defined(EMUCOREA_HAVE_LIBRASHADER)
    Reset();
    {
        VulkanContext context;
        NativeVulkanPresentation presentation(&context);
        Check(presentation.Create(&context), "create presentation");
        presentation.QueuePresent(&context, context.GetGraphicsQueue(), 0, finished);
        CheckDriver("shader output must have the documented color attachment layout");
        Complete(); presentation.Destroy(&context);
        Check(context.swapchainDestroyed && context.surfaceDestroyed, "destroy real swapchain and Android surface");
    }
    Reset();
    {
        VulkanContext context;
        NativeVulkanPresentation presentation(&context);
        presentation.Create(&context);
        presentation.QueuePresent(&context, context.GetGraphicsQueue(), 0, finished);
        errors.clear();
        presentation.QueuePresent(&context, context.GetGraphicsQueue(), 0, finished);
        CheckDriver("wait for a previous shader submit before reusing its command buffer");
        Check(queueWaits == 0, "steady-state presentation must not idle the whole GPU queue");
        Complete(); presentation.Destroy(&context);
    }
    Reset();
    {
        VulkanContext context;
        NativeVulkanPresentation presentation(&context);
        presentation.Create(&context);
        presentation.QueuePresent(&context, context.GetGraphicsQueue(), 0, finished);
        errors.clear();
        emucorer::shader_chain::SetPreset("replacement.slangp", true);
        presentation.QueuePresent(&context, context.GetGraphicsQueue(), 1, finished);
        CheckDriver("hot reload must retire pending shader resources before freeing them");
        Complete(); presentation.Destroy(&context);
    }
    Reset();
    {
        VulkanContext context;
        NativeVulkanPresentation presentation(&context);
        presentation.Create(&context);
        presentation.QueuePresent(&context, context.GetGraphicsQueue(), 0, finished);
        errors.clear();
        context.caps.currentExtent = {16, 16};
        presentation.QueuePresent(&context, context.GetGraphicsQueue(), 1, finished);
        CheckDriver("resizing the shader input must retire every submission using the old image");
        Complete(); presentation.Destroy(&context);
    }
    Reset();
    {
        swapchainImageCount = 4;
        VulkanContext context;
        NativeVulkanPresentation presentation(&context);
        presentation.Create(&context);
        for (uint32_t image = 0; image < 4; ++image) {
            presentation.QueuePresent(&context, context.GetGraphicsQueue(), image, finished);
        }
        CheckDriver("shader residual lifetime must cover every pending swapchain image");
        Complete(); presentation.Destroy(&context);
    }
    Reset();
    {
        VulkanContext context;
        NativeVulkanPresentation presentation(&context);
        presentation.Create(&context);
        presentation.QueuePresent(&context, context.GetGraphicsQueue(), 0, finished);
        errors.clear();
        swapchainImageCount = 4;
        context.swapchain = TestHandle<VkSwapchainKHR>(20);
        for (uint32_t image : {1u, 2u, 3u, 0u}) {
            presentation.QueuePresent(&context, context.GetGraphicsQueue(), image, finished);
        }
        CheckDriver("a recreated swapchain must update the shader resource lifetime ring");
        Complete(); presentation.Destroy(&context);
    }
    Reset();
    {
        VulkanContext context;
        NativeVulkanPresentation presentation(&context);
        presentation.Create(&context); failChainFrame = true;
        presentation.QueuePresent(&context, context.GetGraphicsQueue(), 0, finished);
        presentation.QueuePresent(&context, context.GetGraphicsQueue(), 0, finished);
        Check(chainFrameCalls == 1, "failed preset stays on pass-through until its generation changes");
        Complete(); presentation.Destroy(&context);
    }
#endif
    Reset();
    {
        VulkanContext context;
        NativeVulkanPresentation presentation(&context);
        presentation.Create(&context);
        emucorer::shader_chain::SetPreset("", false);
        Check(presentation.QueuePresent(&context, context.GetGraphicsQueue(), 0, finished) == VK_SUCCESS,
              "disabled shaders still present successfully");
        Check(chainFrameCalls == 0 && pendingCommands.empty() && queueWaits == 0,
              "disabled shaders add no shader submission or queue wait");
        Complete(); presentation.Destroy(&context);
    }
    Reset();
    {
        VulkanContext context;
        NativeVulkanPresentation presentation(&context);
        presentation.Create(&context); failSubmit = true;
        presentation.QueuePresent(&context, context.GetGraphicsQueue(), 0, finished);
        errors.clear();
        presentation.QueuePresent(&context, context.GetGraphicsQueue(), 0, finished);
        CheckDriver("a rejected submit must not leave an unsignaled fence to wait on forever");
        Complete(); presentation.Destroy(&context);
    }
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}

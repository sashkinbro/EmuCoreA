#pragma once
#define VK_NO_PROTOTYPES
#include "ext/vulkan/vulkan.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#define _dbg_assert_(...) ((void)0)
#define _dbg_assert_msg_(...) ((void)0)
#define _assert_(condition) do { if (!(condition)) std::abort(); } while (0)
#define ERROR_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
using VmaAllocator = void *;
using VmaAllocation = void *;
struct VmaAllocationCreateInfo { int usage; };
struct VmaAllocationInfo {};
constexpr int VMA_MEMORY_USAGE_GPU_ONLY = 1;
VkResult vmaCreateImage(VmaAllocator, const VkImageCreateInfo *, const VmaAllocationCreateInfo *, VkImage *, VmaAllocation *, VmaAllocationInfo *);
void TestDeleteView(VkImageView);
void TestDeleteImage(VkImage, VmaAllocation);
template<typename T> T TestHandle(uintptr_t value) { return reinterpret_cast<T>(value); }
namespace PPSSPP_VK {
extern PFN_vkCreateImageView vkCreateImageView;
extern PFN_vkCreateFramebuffer vkCreateFramebuffer;
}
using namespace PPSSPP_VK;
struct TestDeleteList {
    void QueueDeleteImageView(VkImageView view) { TestDeleteView(view); }
    void QueueDeleteImageAllocation(VkImage image, VmaAllocation alloc) { TestDeleteImage(image, alloc); }
    void QueueDeleteFramebuffer(VkFramebuffer) {}
    void QueueDeleteRenderPass(VkRenderPass) {}
};
class VulkanContext {
public:
    struct DeviceInfo { VkFormat preferredDepthStencilFormat = VK_FORMAT_D24_UNORM_S8_UINT; };
    struct Properties { VkPhysicalDeviceProperties properties{}; };
    struct ExtensionInfo { bool EXT_debug_utils = false; };
    VulkanContext() { properties.properties.limits.maxImageDimension2D = 4096; }
    VkDevice GetDevice() const { return TestHandle<VkDevice>(1); }
    VmaAllocator Allocator() const { return nullptr; }
    const DeviceInfo &GetDeviceInfo() const { return deviceInfo; }
    const Properties &GetPhysicalDeviceProperties() const { return properties; }
    const ExtensionInfo &Extensions() const { return extensions; }
    bool DebugLayerEnabled() const { return false; }
    template<typename T> void SetDebugName(T, VkObjectType, const char *) {}
    TestDeleteList &Delete() { return deletes; }
private:
    DeviceInfo deviceInfo;
    Properties properties;
    ExtensionInfo extensions;
    TestDeleteList deletes;
};

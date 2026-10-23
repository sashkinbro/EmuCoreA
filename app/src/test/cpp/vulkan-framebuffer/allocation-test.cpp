#include "Common/GPU/Vulkan/VulkanQueueRunner.h"
#include <map>
#include <set>

static int allocationCalls = 0, viewCalls = 0, failAllocation = 0, failView = 0, driverErrors = 0;
static uintptr_t nextHandle = 10;
static std::set<VkImage> images;
static std::map<VkImageView, VkImage> views;
VkRenderPass VKRRenderPass::Get(VulkanContext *, RenderPassType, VkSampleCountFlagBits) { return TestHandle<VkRenderPass>(1); }
static void Check(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
VkResult vmaCreateImage(VmaAllocator, const VkImageCreateInfo *, const VmaAllocationCreateInfo *,
        VkImage *image, VmaAllocation *alloc, VmaAllocationInfo *) {
    if (++allocationCalls == failAllocation) { *image = VK_NULL_HANDLE; *alloc = nullptr; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    *image = TestHandle<VkImage>(nextHandle++);
    *alloc = reinterpret_cast<void *>(1);
    images.insert(*image);
    return VK_SUCCESS;
}
namespace PPSSPP_VK {
PFN_vkCreateImageView vkCreateImageView = [](VkDevice, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *, VkImageView *view) {
    ++viewCalls;
    *view = VK_NULL_HANDLE;
    if (!images.count(info->image)) { ++driverErrors; return VK_ERROR_INITIALIZATION_FAILED; }
    if (viewCalls == failView) {
        // Failed Vulkan output parameters must not be treated as live handles.
        *view = TestHandle<VkImageView>(0xBAD);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *view = TestHandle<VkImageView>(nextHandle++); views[*view] = info->image; return VK_SUCCESS;
};
PFN_vkCreateFramebuffer vkCreateFramebuffer = [](VkDevice, const VkFramebufferCreateInfo *, const VkAllocationCallbacks *, VkFramebuffer *) { return VK_SUCCESS; };
}
void TestDeleteView(VkImageView view) { Check(views.erase(view) == 1, "invalid or duplicate image view deletion"); }
void TestDeleteImage(VkImage image, VmaAllocation) {
    for (auto &view : views) Check(view.second != image, "image deleted before its views");
    Check(images.erase(image) == 1, "invalid or duplicate image deletion");
}
static void Reset() {
    Check(images.empty() && views.empty(), "leaked partial framebuffer resources");
    allocationCalls = viewCalls = failAllocation = failView = driverErrors = 0;
}
struct FramebufferDesc {
    int width, height, depth, numLayers, multiSampleLevel;
    bool z_stencil;
    const char *tag;
};
struct Framebuffer { virtual ~Framebuffer() = default; };
struct VKFramebuffer : Framebuffer {
    VKRFramebuffer *buffer;
    VKFramebuffer(VKRFramebuffer *buffer, int) : buffer(buffer) {}
    ~VKFramebuffer() override { delete buffer; }
};
struct VKContext {
    VulkanContext *vulkan_;
    struct RenderManager {
        VulkanBarrierBatch barriers;
        VulkanBarrierBatch &PostInitBarrier() { return barriers; }
    } renderManager_;
    Framebuffer *CreateFramebuffer(const FramebufferDesc &desc);
};
#include "framebuffer-factory.inc"

struct TestVfb {
    int renderWidth = 480, renderHeight = 272;
    int width = 480, height = 272, renderScaleFactor = 1, usageFlags = 1;
    bool multisampled = false;
    Framebuffer *fbo = nullptr;
    int last_frame_failed = 0;
};
struct TestGpuStats { struct Totals { int numFlips = 77; } totals; } gpuStats;
struct ResizeHarness {
    struct DrawContext {
        Framebuffer *result = nullptr;
        Framebuffer *CreateFramebuffer(const FramebufferDesc &) { return result; }
    } draw;
    DrawContext *draw_ = &draw;
    int msaaLevel_ = 0;
    int GetFramebufferLayers() { return 1; }
    bool continued = false;
    bool Resize(TestVfb *vfb) {
        TestVfb old = *vfb;
        const char *tag = "resize";
        vfb->renderWidth = 960;
        vfb->renderHeight = 544;
#include "resize-allocation.inc"
        continued = true;
        return true;
    }
};
constexpr int FB_USAGE_COLOR_MIXED_DEPTH = 1;
struct TestCompat {
    struct Flags { bool ForceLowerResolutionForEffectsOn = false; } values;
    const Flags &flags() const { return values; }
};
struct TestCoreParameter { TestCompat compat; };
static TestCoreParameter &PSP_CoreParameter() { static TestCoreParameter parameter; return parameter; }
struct ResolutionUpgradeHarness : ResizeHarness {
    bool ResizeFramebufFBO(TestVfb *vfb, int, int, bool) {
        if (!Resize(vfb)) return false;
        vfb->renderScaleFactor = 2;
        return true;
    }
    void Upgrade(TestVfb *vfb) {
#include "resolution-upgrade.inc"
    }
};
int main() {
    VulkanContext vulkan;
    // A rejected image allocation must never reach vkCreateImageView.
    Reset(); failAllocation = 1;
    {
        VulkanBarrierBatch barriers;
        VKRFramebuffer framebuffer(&vulkan, &barriers, 480, 272, 2, 2, true, "allocation failure");
        Check(!framebuffer.IsValid(), "failed framebuffer reported success");
        Check(driverErrors == 0, "vkCreateImageView received an invalid image after allocation failure");
        Check(viewCalls == 0, "continued creating views after allocation failed");
        Check(barriers.images.empty(), "failed framebuffer published image barriers");
    }
    Reset();
    for (int failure = 1; failure <= 4; ++failure) {
        failAllocation = failure;
        {
            VulkanBarrierBatch barriers;
            VKRFramebuffer framebuffer(&vulkan, &barriers, 480, 272, 2, 2, true, "partial attachment failure");
            Check(!framebuffer.IsValid(), "partial attachment failure reported success");
            Check(allocationCalls == failure, "allocated attachments after a failure");
            Check(driverErrors == 0, "invalid image reached the driver");
            Check(barriers.images.empty(), "partial framebuffer left dangling image barriers");
        }
        Reset();
    }
    for (int failure = 1; failure <= 16; ++failure) {
        failView = failure;
        {
            VulkanBarrierBatch barriers;
            VKRFramebuffer framebuffer(&vulkan, &barriers, 480, 272, 2, 2, true, "partial view failure");
            Check(!framebuffer.IsValid(), "partial view failure reported success");
            Check(viewCalls == failure, "created views after a failure");
            Check(barriers.images.empty(), "failed view left dangling image barriers");
        }
        Reset();
    }
    for (int layers : {1, 2}) for (int msaa : {0, 2}) for (bool depth : {false, true}) {
        VulkanBarrierBatch barriers;
        {
            VKRFramebuffer framebuffer(&vulkan, &barriers, 480, 272, layers, msaa, depth, "success");
            const size_t count = (depth ? 2 : 1) * (msaa ? 2 : 1);
            Check(framebuffer.IsValid(), "valid framebuffer creation failed");
            Check(images.size() == count && barriers.images.size() == count, "missing attachments or transitions");
            Check(views.size() == count * (2 + layers), "missing attachment views");
            Check(framebuffer.color.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, "color layout not initialized");
            if (depth) Check(framebuffer.depth.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, "depth layout not initialized");
            barriers.images.clear();
        }
        Reset();
    }
    for (int size : {0, -1, 4097}) {
        VulkanBarrierBatch barriers;
        {
            VKRFramebuffer framebuffer(&vulkan, &barriers, size, 272, 1, 0, false, "invalid size");
            Check(!framebuffer.IsValid(), "invalid image dimensions accepted");
            Check(allocationCalls == 0 && viewCalls == 0, "invalid dimensions reached the driver");
        }
        Reset();
    }
    {
        VKContext context{&vulkan, {}};
        failAllocation = 2;
        Framebuffer *failed = context.CreateFramebuffer({480, 272, 1, 1, 0, true, "factory failure"});
        Check(failed == nullptr, "factory published an incomplete framebuffer");
        Check(context.renderManager_.barriers.images.empty(), "factory failure published barriers");
        Reset();
        Framebuffer *success = context.CreateFramebuffer({480, 272, 1, 1, 0, true, "factory success"});
        Check(success != nullptr, "factory rejected a valid framebuffer");
        context.renderManager_.barriers.images.clear();
        delete success;
        Reset();
    }
    {
        Framebuffer oldBuffer, newBuffer;
        for (Framebuffer *old : {&oldBuffer, static_cast<Framebuffer *>(nullptr)}) {
            TestVfb vfb; vfb.fbo = old;
            ResizeHarness harness;
            Check(!harness.Resize(&vfb), "failed resize reported success");
            Check(vfb.fbo == old && vfb.renderWidth == 480 && vfb.renderHeight == 272,
                "failed resize discarded the old target or changed its dimensions");
            Check(vfb.last_frame_failed == 77, "failed resize did not record failure");
            Check(!harness.continued, "failed resize continued to framebuffer binds and copies");
        }
        TestVfb vfb;
        ResizeHarness harness; harness.draw.result = &newBuffer;
        Check(harness.Resize(&vfb), "valid resize reported failure");
        Check(harness.continued && vfb.fbo == &newBuffer && vfb.renderWidth == 960, "successful resize did not continue");
        TestVfb nativeTarget; nativeTarget.fbo = &oldBuffer;
        ResolutionUpgradeHarness promotion;
        promotion.Upgrade(&nativeTarget);
        Check(nativeTarget.fbo == &oldBuffer && nativeTarget.renderScaleFactor == 1,
            "failed resolution upgrade lost the native target");
        promotion.draw.result = &newBuffer;
        promotion.Upgrade(&nativeTarget);
        Check(nativeTarget.fbo == &newBuffer && nativeTarget.renderScaleFactor == 2,
            "successful resolution upgrade did not update the target");
    }
    std::puts("Vulkan framebuffer allocations, partial cleanup, transitions, and limits: PASS");
}

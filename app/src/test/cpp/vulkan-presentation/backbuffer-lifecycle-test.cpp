// The runner includes DestroyBackbuffers directly from the production source.
// Model deferred deletes and GPU submissions to verify resource lifetime ordering.
#include <cstdio>
#include <cstdlib>
#include <vector>

static int pendingBarrier = 0, recorded = 0, submitted = 0, liveDepth = 0;
static int uploads = 0, submittedUploads = 0, completedUploads = 0, waits = 0;
static void Require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
enum class FrameSubmitType { Pending };
struct VulkanContext {
    int GetCurFrame() { return 0; }
    int GetDevice() { return 1; }
    void WaitUntilQueueIdle() { submitted = 0; completedUploads += submittedUploads; submittedUploads = 0; ++waits; }
    VulkanContext &Delete() { return *this; }
    void QueueDeleteImageView(int) {}
};
struct FrameDataShared {
    struct Image { int view = 1, renderingCompleteSemaphore = 1; };
    std::vector<Image> swapchainImages_;
    int swapchainImageCount_ = 0;
};
struct FrameData {
    int GetInitCmd(VulkanContext *) { return 1; }
    void Submit(VulkanContext *, FrameSubmitType, FrameDataShared &) {
        submitted += recorded; recorded = 0;
        submittedUploads += uploads; uploads = 0;
    }
};
struct BarrierBatch {
    bool empty() { return pendingBarrier == 0; }
    void Flush(int) {
        Require(pendingBarrier == liveDepth, "recorded a barrier for a destroyed depth image");
        recorded = pendingBarrier;
        pendingBarrier = 0;
    }
};
struct QueueRunner {
    void DestroyBackBuffers() {
        Require(pendingBarrier == 0, "destroyed depth image with an unrecorded barrier");
        Require(recorded == 0 && submitted == 0, "destroyed depth image before initialization completed");
        Require(uploads == 0 && submittedUploads == 0, "lost pending texture uploads during backbuffer teardown");
        liveDepth = 0;
    }
};
void vkDestroySemaphore(int, int, void *) {}
struct VulkanRenderManager {
    VulkanContext context;
    VulkanContext *vulkan_ = &context;
    FrameData frameData_[1];
    FrameDataShared frameDataShared_;
    BarrierBatch postInitBarrier_;
    QueueRunner queueRunner_;
    void StopThreads() {}
    void DestroyBackbuffers();
};
#include "destroy-backbuffers.inc"

int main() {
    VulkanRenderManager manager;
    // Initialization followed by a resize/presentation change before Finish().
    for (int i = 1; i <= 3; ++i) {
        liveDepth = pendingBarrier = i;
        uploads = 1;
        manager.frameDataShared_.swapchainImages_.push_back({});
        manager.frameDataShared_.swapchainImageCount_ = 1;
        manager.DestroyBackbuffers();
        Require(manager.frameDataShared_.swapchainImages_.empty(), "old swapchain views retained");
        Require(manager.frameDataShared_.swapchainImageCount_ == 0, "old swapchain count retained");
    }
    // Uploads must still complete when no image barriers remain.
    uploads = 1;
    manager.DestroyBackbuffers();
    // Repeated teardown after failed recreation, with no barrier or images.
    manager.DestroyBackbuffers();
    Require(waits == 5 && completedUploads == 4, "initialization was not drained on every teardown");
    std::puts("Vulkan backbuffer initialization lifetime: PASS");
}

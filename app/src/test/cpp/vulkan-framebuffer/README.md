# Vulkan framebuffer allocation regressions

Run `./app/src/test/cpp/vulkan-framebuffer/run.ps1` from the repository root on Windows. Pass `-Cxx` to select another host C++17 compiler.

The runner compiles the framebuffer/image implementation extracted from production `VulkanFramebuffer.cpp` before its independent render-pass implementation. It uses the real framebuffer types and vendored Vulkan API types with deterministic allocator, driver, context, and barrier doubles. Debug assertions are disabled to exercise release error handling.

Tests inject failures at every image allocation and image-view creation in a stereo MSAA framebuffer, check partial-resource cleanup and that no failed framebuffer publishes transitions, and verify successful mono/stereo, color/depth, MSAA, and image-size limits. This does not reproduce a particular vendor driver's crash or model real GPU execution.

The production thin3d framebuffer factory, resize allocation/rollback block, and native-resolution upgrade caller are also extracted and compiled. Tests check that failure returns null, frees partial attachments, retains an existing resize target and its dimensions, returns before bind/copy operations, and avoids the success-only scale assertion after a failed upgrade. The surrounding framebuffer-manager rendering state is not modeled by this host test.

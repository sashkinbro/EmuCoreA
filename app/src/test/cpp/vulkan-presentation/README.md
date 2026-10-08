# Vulkan presentation contract regressions

`run.ps1` compiles the actual `NativeVulkanPresentation` and shader-selection implementation with a host C++17 compiler. It runs both the shader-enabled and shader-disabled builds. It uses the vendored Vulkan headers and the configured project's pinned librashader C header, so signatures remain checked against the shipped dependency.

The Vulkan and librashader dispatch doubles represent the GPU boundary. Submissions remain pending until the application waits for their fence or the queue. They reject resetting a pending command buffer, freeing an in-use shader chain/image, and passing an output image in a layout prohibited by librashader. The tests also cover failed shader generations, failed submissions, disabled pass-through, and real-swapchain cleanup.

From the repository root on Windows:

```powershell
./app/src/test/cpp/vulkan-presentation/run.ps1
```

Pass `-Cxx` or `-LibrashaderIncludeDirectory` to override discovery. These tests verify host synchronization and API contracts; device rendering/color correctness remains covered by the separate GPU probes and Android instrumentation tests.

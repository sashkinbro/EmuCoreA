# GPU color regression probes

These isolated Android executables use the pinned production librashader binary and an identity preset. They do not launch the application or write game data.

- `gl-test.cpp` links the production `shader_chain_present.cpp` and `shader_chain.cpp`, plus `core-gl-dispatch-stubs.cpp` to reproduce PPSSPP's GLES3 function-pointer data symbols (direct GLES3 calls collide with these symbols). Its EGL ES3 pbuffer checks RGB with an inherited partial color mask, capture from framebuffer 0, asymmetric image orientation, resize, live disable/re-enable, framebuffer cache restoration, sticky frame failure and recovery, invalid preset fallback, requested input mipmaps, and EGL context destruction/recreation. Successful frames use the shipped librashader and real GLES driver; a linker wrapper injects one genuine librashader error for the failure regression.
- `vk-test.cpp` creates a BGRA source image and proves the old RGBA declaration swaps red/blue. The actual BGRA declaration must preserve RGB. The mutable-format flag is only for reproducing the old invalid interpretation in the test.
- Readbacks and queue waits exist only in these test executables, never in the app rendering fix.

Build with the Android NDK C++17 compiler, the generated librashader include directory and `liblibrashader_capi.so`. GL requires `EMUCOREA_HAVE_LIBRASHADER`, `LIBRA_RUNTIME_OPENGL`, `-lEGL -lGLESv3 -llog` and `-Wl,--wrap=libra_gl_filter_chain_frame`; Vulkan requires `LIBRA_RUNTIME_VULKAN`, `-lvulkan -llog`. Use shared libc++ to match librashader. Place the executable, `identity.slangp`, `identity.slang`, `mipmap.slangp`, `mipmap.slang`, libc++_shared.so and librashader library in an isolated test directory, then run with `LD_LIBRARY_PATH=.` and the absolute `identity.slangp` path as the argument.

Verified on OnePlus CPH2747 on 2026-10-08: `GL presentation: 0 failures`. Before the fixes, framebuffer restoration and repeated shader failure produced seven failures. The added mipmap regression then reproduced blue `(0,0,255)` from the base checkerboard instead of its level-1 average `(128,0,128)`; the corrected production patch passes both that check and the next-frame changed image.

The production GL swap callback queries two framebuffer bindings only while filtering, clears sampler bindings for the core's eight texture slots, and performs two GPU blits around the shader chain. It never reads pixels or waits for GPU completion. librashader queries texture parameters for shader bindings and generates mipmaps only when the preset requests them. Disabled shaders retain direct swapping without GL state queries or extra draws. These probes establish transport/lifecycle behavior, not a frame-rate benchmark or compatibility with every third-party preset.

Vulkan color probe verified on Lenovo TB710FU on 2026-10-08:

```
Vulkan old RGBA: (51,102,204)
Vulkan actual BGRA: (204,102,51)
```

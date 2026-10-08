#pragma once

namespace emucorea::shader_chain_present {

// Call on the render thread with its GLES3 EGL context current. Source uses GL
// bottom-left coordinates; destination uses top-left window coordinates.
// Restores the core's cached framebuffer bindings and next-frame GL baseline.
bool Present(unsigned int source_fbo, int source_x, int source_y, int source_width, int source_height,
             int window_width, int window_height,
             int destination_x, int destination_y, int destination_width, int destination_height);
// Must run before releasing/destroying that EGL context.
void Destroy();

}  // namespace emucorea::shader_chain_present

// PPSSPP's gl3stub exports GLES3 entry points as data symbols. Keep those
// symbols in the standalone probe, so direct calls into them fail just as they
// would in the integrated core. Production presentation must load local GLES3
// entry points instead of resolving these names as functions.
extern "C" {
void *glBlitFramebuffer = nullptr;
void *glBindVertexArray = nullptr;
void *glBindSampler = nullptr;
}

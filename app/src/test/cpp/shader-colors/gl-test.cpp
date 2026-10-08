#include "shader_chain.h"
#include "shader_chain_present.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <librashader.h>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int failures = 0;
int frameCalls = 0;
bool injectFrameFailure = false;
const char *presetPath = nullptr;

void Check(bool success, const char *description) {
  if (!success) {
    ++failures;
    printf("FAIL: %s\n", description);
  }
}

void CheckPixel(int x, int y, int red, int green, int blue, const char *description) {
  unsigned char pixel[4]{};
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  const bool match = abs(int(pixel[0]) - red) <= 2 &&
                     abs(int(pixel[1]) - green) <= 2 &&
                     abs(int(pixel[2]) - blue) <= 2;
  Check(match, description);
  if (!match) printf("  RGB=%d,%d,%d expected=%d,%d,%d\n", pixel[0], pixel[1], pixel[2], red, green, blue);
}

void ClearDefault(int width, int height) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(0, 0, width, height);
  glClearColor(0.8f, 0.4f, 0.2f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
}

bool PresentDefault(int width = 8, int height = 8) {
  return emucorea::shader_chain_present::Present(0, 0, 0, width, height,
                                                width, height, 0, 0, width, height);
}
}

// Inject a real librashader error at the dependency boundary. All successful
// frames still use the shipped library and an actual GLES driver.
extern "C" libra_error_t __real_libra_gl_filter_chain_frame(
    libra_gl_filter_chain_t *, size_t, libra_image_gl_t, libra_image_gl_t,
    const libra_viewport_t *, const float *, const frame_gl_opt_t *);
extern "C" libra_error_t __wrap_libra_gl_filter_chain_frame(
    libra_gl_filter_chain_t *chain, size_t frame, libra_image_gl_t input,
    libra_image_gl_t output, const libra_viewport_t *viewport,
    const float *mvp, const frame_gl_opt_t *options) {
  ++frameCalls;
  if (injectFrameFailure) {
    libra_shader_preset_t missing = nullptr;
    return libra_preset_create("/this/preset/does/not/exist.slangp", &missing);
  }
  return __real_libra_gl_filter_chain_frame(chain, frame, input, output, viewport, mvp, options);
}

int main(int argc, char **argv) {
  if (argc != 2) return 1;
  presetPath = argv[1];
  EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint a, b, n;
  if (!eglInitialize(d, &a, &b))
    return 2;
  EGLint cfg[] = {EGL_SURFACE_TYPE,
                  EGL_PBUFFER_BIT,
                  EGL_RENDERABLE_TYPE,
                  EGL_OPENGL_ES3_BIT,
                  EGL_RED_SIZE,
                  8,
                  EGL_GREEN_SIZE,
                  8,
                  EGL_BLUE_SIZE,
                  8,
                  EGL_ALPHA_SIZE,
                  8,
                  EGL_NONE};
  EGLConfig c;
  eglChooseConfig(d, cfg, &c, 1, &n);
  EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 12, EGL_NONE};
  auto s = eglCreatePbufferSurface(d, c, size);
  EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  auto x = eglCreateContext(d, c, EGL_NO_CONTEXT, ctx);
  if (!eglMakeCurrent(d, s, s, x))
    return 3;
  GLuint tex, fbo;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         tex, 0);
  glClearColor(0.8f, 0.4f, 0.2f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  emucorer::shader_chain::SetPreset(argv[1], true);
  for (int i = 0; i < 3; i++) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glClearColor(i == 1 ? 0.2f : 0.8f, i == 1 ? 0.8f : 0.4f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(i == 1 ? GL_FALSE : GL_TRUE, i == 1 ? GL_FALSE : GL_TRUE,
                GL_TRUE, GL_TRUE);
    if (!emucorea::shader_chain_present::Present(fbo, 0, 0, 8, 8, 8, 8, 0, 0, 8,
                                                 8)) {
      printf("Present failed\n");
      return 4;
    }
    GLint drawFbo = 0, readFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
    Check(drawFbo == int(fbo) && readFbo == int(fbo), "restore framebuffer bindings cached by PPSSPP");
    CheckPixel(4, 4, i == 1 ? 51 : 204, i == 1 ? 204 : 102, 51,
               "identity shader preserves RGB with inherited color mask");
  }
  ClearDefault(8, 8);
  glEnable(GL_SCISSOR_TEST);
  glScissor(0, 0, 8, 4);
  glClearColor(0.2f, 0.8f, 0.2f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
  Check(PresentDefault(), "filter the real default framebuffer used by swap callback");
  CheckPixel(4, 1, 51, 204, 51, "default-framebuffer capture keeps lower half orientation");
  CheckPixel(4, 6, 204, 102, 51, "default-framebuffer capture keeps upper half orientation");

  ClearDefault(16, 12);
  Check(PresentDefault(16, 12), "resize input and output textures during a running chain");
  CheckPixel(15, 11, 204, 102, 51, "resized frame covers its full surface");

  emucorer::shader_chain::SetPreset("", false);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  const int beforeDisabled = frameCalls;
  Check(!PresentDefault(), "disabled shader uses direct presentation");
  GLint binding = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &binding);
  Check(binding == int(fbo) && frameCalls == beforeDisabled, "disabled shader does not mutate GL or invoke librashader");

  emucorer::shader_chain::SetPreset(presetPath, true);
  ClearDefault(8, 8);
  Check(PresentDefault(), "re-enable the same preset live");
  CheckPixel(4, 4, 204, 102, 51, "live re-enable preserves image");

  injectFrameFailure = true;
  const int beforeFailed = frameCalls;
  emucorer::shader_chain::SetPreset(presetPath, true);
  for (int attempt = 0; attempt < 3; ++attempt) {
    ClearDefault(8, 8);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    Check(!PresentDefault(), "failed shader frame stays on direct presentation");
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &binding);
    Check(binding == int(fbo), "restore cached framebuffer binding on shader failure");
    CheckPixel(4, 4, 204, 102, 51, "shader failure leaves the original frame intact");
  }
  Check(frameCalls == beforeFailed + 1, "do not retry a failed preset every frame");
  injectFrameFailure = false;
  emucorer::shader_chain::SetPreset(presetPath, true);
  ClearDefault(8, 8);
  Check(PresentDefault(), "new preset generation retries after an error");

  const std::string mipmapPath = std::string(presetPath).substr(0, std::string(presetPath).find_last_of('/') + 1) + "mipmap.slangp";
  emucorer::shader_chain::SetPreset(mipmapPath, true);
  ClearDefault(8, 8);
  glEnable(GL_SCISSOR_TEST);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      glScissor(x, y, 1, 1);
      glClearColor((x + y) % 2 ? 1.0f : 0.0f, 0, (x + y) % 2 ? 0.0f : 1.0f, 1);
      glClear(GL_COLOR_BUFFER_BIT);
    }
  }
  glDisable(GL_SCISSOR_TEST);
  Check(PresentDefault(), "preset requests mipmaps of the captured core framebuffer");
  CheckPixel(4, 4, 128, 0, 128, "mipmap sampler sees averaged level 1 rather than base checkerboard");
  ClearDefault(8, 8);
  Check(PresentDefault(), "regenerate requested input mipmaps on the next frame");
  CheckPixel(4, 4, 204, 102, 51, "mipmap content follows the changed core image");

  emucorer::shader_chain::SetPreset("/this/preset/does/not/exist.slangp", true);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  Check(!PresentDefault(), "invalid preset uses direct presentation");
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &binding);
  Check(binding == int(fbo), "preset load failure restores framebuffer bindings");

  Check(glGetError() == GL_NO_ERROR, "shader presentation leaves no GL errors");
  emucorea::shader_chain_present::Destroy();
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &tex);
  eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(d, x);
  x = eglCreateContext(d, c, EGL_NO_CONTEXT, ctx);
  Check(eglMakeCurrent(d, s, s, x), "recreate EGL context after presentation teardown");
  emucorer::shader_chain::SetPreset(presetPath, true);
  ClearDefault(8, 8);
  Check(PresentDefault(), "create shader resources in replacement EGL context");
  CheckPixel(4, 4, 204, 102, 51, "replacement EGL context renders the same identity image");
  Check(glGetError() == GL_NO_ERROR, "replacement context has no stale GL handles");
  emucorea::shader_chain_present::Destroy();
  eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(d, x);
  eglDestroySurface(d, s);
  eglTerminate(d);
  printf("GL presentation: %d failures\n", failures);
  return failures ? 5 : 0;
}

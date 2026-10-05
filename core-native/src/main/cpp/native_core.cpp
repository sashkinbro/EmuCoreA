// EmuCoreA native PPSSPP frontend.
//
// Drives the vendored PPSSPP core directly (Core/GPU/Common), with the same
// graphics and audio backends the standalone PPSSPP app uses, but without the
// libretro wrapper and without PPSSPP's ImGui UI. EmuCoreA's own Compose UI
// talks to this file through JNI.
//
// Responsibilities:
//   * System_* callbacks the core expects from its platform layer
//   * Vulkan graphics context bound to an Android Surface
//   * Core boot (PSP_Init), per-frame emulation loop and presentation
//   * Audio output through PPSSPP's StereoResampler + OpenSL backend
//   * Input, save states, shutdown
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <sys/system_properties.h>

#include <memory>
#include <string>
#include <vector>

#include "Common/CPUDetect.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/File/VFS/VFS.h"
#include "Common/File/VFS/ZipFileReader.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/Vulkan/VulkanGraphicsContext.h"
#include "Common/Log.h"
#include "Common/System/Display.h"
#include "Common/System/System.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/Thread/ThreadUtil.h"

#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreParameter.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceDisplay.h"
#include "Core/HW/StereoResampler.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "GPU/GPUCommon.h"

#include "android/jni/AndroidAudio.h"

#define LOG_TAG "EmuCoreA-Native"
#define NLOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define NLOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define NLOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

JavaVM *g_vm = nullptr;
GraphicsContext *g_graphicsContext = nullptr;
StereoResampler g_resampler;
AndroidAudioState *g_audioState = nullptr;
bool g_audioStarted = false;
bool g_booted = false;
bool g_pendingBoot = false;
bool g_renderReady = false;
std::string g_bootError;
int g_displayWidth = 1080;
int g_displayHeight = 1920;
float g_displayRefreshRate = 60.0f;

std::string ToString(JNIEnv *env, jstring value) {
    if (value == nullptr) return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars != nullptr ? chars : "";
    if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
    return out;
}

// ---------------------------------------------------------------------------
// Audio. Mirrors PPSSPP's UI/AudioCommon.cpp path: the emulator pushes mixed
// samples into StereoResampler, the OpenSL callback pulls device-rate frames.
// ---------------------------------------------------------------------------
void AudioRenderCallback(short *buffer, int numSamples, int sampleRateHz, void *userdata) {
    g_resampler.Mix(buffer, numSamples, false, sampleRateHz);
}

void StartAudio() {
    if (g_audioState == nullptr) {
        g_audioState = AndroidAudio_Init(AudioRenderCallback, 0, 44100);
    }
    if (!g_audioStarted && g_audioState != nullptr) {
        g_audioStarted = AndroidAudio_Resume(g_audioState);
        NLOGI("Audio started: %d", g_audioStarted ? 1 : 0);
    }
}

void StopAudio() {
    if (g_audioState != nullptr && g_audioStarted) {
        AndroidAudio_Pause(g_audioState);
        g_audioStarted = false;
    }
}

}  // namespace

// JNI helpers the core's Android text renderer expects from the platform layer.
JNIEnv *getEnv() {
    if (g_vm == nullptr) return nullptr;
    JNIEnv *env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    }
    return env;
}

jclass findClass(const char *name) {
    JNIEnv *env = getEnv();
    return env != nullptr ? env->FindClass(name) : nullptr;
}

// ---------------------------------------------------------------------------
// System callbacks required by the core.
// ---------------------------------------------------------------------------
void System_AudioPushSamples(const int32_t *audio, int numSamples, float volume) {
    if (audio != nullptr) {
        g_resampler.PushSamples(audio, numSamples, volume);
    } else {
        g_resampler.Clear();
    }
}

void System_AudioGetDebugStats(char *buf, size_t bufSize) {
    if (buf != nullptr) {
        g_resampler.GetAudioDebugStats(buf, bufSize);
    } else {
        g_resampler.ResetStatCounters();
    }
}

void System_AudioClear() {
    g_resampler.Clear();
}

#if PPSSPP_PLATFORM(ANDROID)
bool System_AudioRecordingIsAvailable() { return false; }
bool System_AudioRecordingState() { return false; }
#endif

int64_t System_GetPropertyInt(SystemProperty prop) {
    switch (prop) {
    case SYSPROP_AUDIO_SAMPLE_RATE:
        return 44100;
    case SYSPROP_DISPLAY_XRES:
        return g_displayWidth;
    case SYSPROP_DISPLAY_YRES:
        return g_displayHeight;
#if PPSSPP_PLATFORM(ANDROID)
    case SYSPROP_SYSTEMVERSION: {
        char sdk[PROP_VALUE_MAX] = {0};
        if (__system_property_get("ro.build.version.sdk", sdk) != 0) {
            return atoi(sdk);
        }
        return -1;
    }
#endif
    default:
        break;
    }
    return -1;
}

float System_GetPropertyFloat(SystemProperty prop) {
    switch (prop) {
    case SYSPROP_DISPLAY_REFRESH_RATE:
        return g_displayRefreshRate;
    case SYSPROP_DISPLAY_SAFE_INSET_LEFT:
    case SYSPROP_DISPLAY_SAFE_INSET_RIGHT:
    case SYSPROP_DISPLAY_SAFE_INSET_TOP:
    case SYSPROP_DISPLAY_SAFE_INSET_BOTTOM:
        return 0.0f;
    default:
        break;
    }
    return -1;
}

bool System_GetPropertyBool(SystemProperty prop) {
    switch (prop) {
    case SYSPROP_CAN_JIT:
        return true;
    default:
        return false;
    }
}

std::string System_GetProperty(SystemProperty prop) { return ""; }
std::vector<std::string> System_GetPropertyStringVec(SystemProperty prop) { return {}; }

void System_Notify(SystemNotification notification) {
    switch (notification) {
    default:
        break;
    }
}

bool System_MakeRequest(SystemRequestType type, int requestId, const std::string &param1,
                        const std::string &param2, int64_t param3, int64_t param4) {
    return false;
}

void System_PostUIMessage(UIMessage message, std::string_view param) {}
void System_RunOnMainThread(std::function<void()>) {}
void NativeFrame(GraphicsContext *graphicsContext) {}
void NativeResized() {}
void System_Toast(std::string_view str) {}
void System_LaunchUrl(LaunchUrlType urlType, std::string_view url) {}
std::vector<std::string> System_GetCameraDeviceList() { return {}; }

bool NativeSaveSecret(std::string_view nameOfSecret, std::string_view data) { return false; }
std::string NativeLoadSecret(std::string_view nameOfSecret) { return ""; }

// ---------------------------------------------------------------------------
// Boot / render.
// ---------------------------------------------------------------------------
namespace {

bool CreateGraphicsContext() {
    if (g_graphicsContext != nullptr) return true;
    g_graphicsContext = new VulkanGraphicsContext();
    std::string deviceName;
    std::string error;
    if (!g_graphicsContext->InitAPI(nullptr, &deviceName, &error)) {
        NLOGE("Vulkan InitAPI failed: %s", error.c_str());
        delete g_graphicsContext;
        g_graphicsContext = nullptr;
        return false;
    }
    Core_SetGraphicsContext(g_graphicsContext);
    SetGPUBackend(GPUBackend::VULKAN);
    NLOGI("Vulkan API initialized (%s)", deviceName.c_str());
    return true;
}

bool BootGame(const std::string &gamePath) {
    if (g_graphicsContext == nullptr) return false;

    CoreParameter coreParam{};
    coreParam.enableSound = true;
    coreParam.fileToStart = Path(gamePath);
    coreParam.startBreak = false;
    coreParam.headLess = false;
    coreParam.graphicsContext = g_graphicsContext;
    coreParam.gpuCore = GPUCORE_VULKAN;
    coreParam.cpuCore = CPUCore::JIT;
    coreParam.bUseVertexDecoderJit = true;

    // Asynchronous boot, exactly like the libretro wrapper: the loader thread
    // runs in the background and PSP_InitUpdate is polled from the frame loop
    // once the surface (and with it the draw context) is ready.
    if (!PSP_InitStart(coreParam)) {
        NLOGE("PSP_InitStart failed: %s", coreParam.errorString.c_str());
        return false;
    }
    g_bootError.clear();
    g_pendingBoot = true;
    NLOGI("Game boot started: %s", gamePath.c_str());
    return true;
}

bool AttachSurface(ANativeWindow *window, int width, int height) {
    if (g_graphicsContext == nullptr) return false;
    std::string error;
    if (!g_graphicsContext->InitSurface(WINDOWSYSTEM_ANDROID, window, nullptr, &error)) {
        NLOGE("InitSurface failed: %s", error.c_str());
        return false;
    }
    // InitSurface() already created the swapchain, the draw context, its
    // presets and the backbuffer event. Calling any of those again starts the
    // render threads twice, which terminates the process.
    if (g_graphicsContext->GetDrawContext() == nullptr) return false;
    g_renderReady = true;
    NLOGI("Surface attached %dx%d", width, height);
    return true;
}

void RunFrame() {
    if (g_graphicsContext == nullptr || !g_renderReady) return;

    if (g_pendingBoot) {
        BootState state = PSP_InitUpdate(&g_bootError);
        switch (state) {
        case BootState::Failed:
            NLOGE("PSP_InitUpdate failed: %s", g_bootError.c_str());
            g_pendingBoot = false;
            return;
        case BootState::Booting:
            return;
        case BootState::Complete:
            NLOGI("Game boot complete");
            g_pendingBoot = false;
            g_booted = true;
            break;
        case BootState::Off:
        default:
            return;
        }
    }

    if (!g_booted || gpu == nullptr) return;

    Draw::DrawContext *draw = g_graphicsContext->GetDrawContext();
    if (draw != nullptr) {
        draw->BeginFrame(Draw::DebugFlags::NONE);
    }

    const DisplayLayoutConfig &layout = g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation());
    gpu->BeginHostFrame(layout);
    PSP_RunLoopWhileState();
    switch (coreState) {
    case CORE_NEXTFRAME:
    case CORE_POWERDOWN:
        coreState = CORE_RUNNING_CPU;
        break;
    default:
        break;
    }
    gpu->EndHostFrame();
    if (draw != nullptr) {
        gpu->PrepareCopyDisplayToOutput(layout);
    }

    if (draw != nullptr) {
        using namespace Draw;
        draw->BindFramebufferAsRenderTarget(nullptr, {RPAction::CLEAR, RPAction::CLEAR, RPAction::CLEAR}, "BackBuffer");
    }
    gpu->CopyDisplayToOutput(layout);

    if (draw != nullptr) {
        draw->EndFrame();
        draw->Present(Draw::PresentMode::FIFO);
    }
    g_graphicsContext->Poll();
}

void ShutdownCore() {
    StopAudio();
    if (g_audioState != nullptr) {
        AndroidAudio_Shutdown(g_audioState);
        g_audioState = nullptr;
    }
    if (g_booted) {
        PSP_Shutdown(true);
        g_booted = false;
    }
    if (g_graphicsContext != nullptr) {
        if (g_renderReady) {
            if (gpu != nullptr) gpu->DeviceLost();
            g_graphicsContext->ShutdownSurface();
            g_renderReady = false;
        }
        g_graphicsContext->ShutdownAPI();
        delete g_graphicsContext;
        g_graphicsContext = nullptr;
    }
    if (g_threadManager.IsInitialized()) {
        g_threadManager.Teardown();
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// JNI surface for com.sbro.emucorea.core.NativePpsspp.
// ---------------------------------------------------------------------------
extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    g_vm = vm;
    NLOGI("emucorea native core loaded");
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeInit(JNIEnv *env, jclass, jstring apkPath,
                                                    jstring dataDir, jstring externalDir,
                                                    jstring cacheDir, jint displayWidth,
                                                    jint displayHeight, jfloat refreshRate) {
    g_displayWidth = displayWidth > 0 ? displayWidth : 1080;
    g_displayHeight = displayHeight > 0 ? displayHeight : 1920;
    g_displayRefreshRate = refreshRate > 0.0f ? refreshRate : 60.0f;

    const std::string apk = ToString(env, apkPath);
    const std::string data = ToString(env, dataDir);
    const std::string external = ToString(env, externalDir);
    const std::string cache = ToString(env, cacheDir);

    if (!apk.empty()) {
        g_VFS.Register("", ZipFileReader::Create(Path(apk), "assets/"));
    }
    if (!data.empty()) {
        g_Config.internalDataDirectory = Path(data);
    }
    if (!external.empty()) {
        g_Config.memStickDirectory = Path(external);
    }
    g_Config.nandRootDirectory = GetSysDirectory(DIRECTORY_NAND);
    CreateSysDirectories();
    if (!g_threadManager.IsInitialized()) {
        g_threadManager.Init(cpu_info.num_cores, cpu_info.logical_cpu_count);
    }
    NLOGI("nativeInit apk=%s data=%s external=%s", apk.c_str(), data.c_str(), external.c_str());
    CreateGraphicsContext();
}

JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeBoot(JNIEnv *env, jclass, jstring gamePath) {
    const std::string game = ToString(env, gamePath);
    if (game.empty()) return JNI_FALSE;
    // The non-libretro loader thread initializes the GPU immediately, so the
    // draw context (created with the surface) must already exist.
    if (!g_renderReady) {
        NLOGE("nativeBoot called before the surface was attached");
        return JNI_FALSE;
    }
    StartAudio();
    return BootGame(game) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetSurface(JNIEnv *env, jclass, jobject surface,
                                                          jint width, jint height) {
    if (surface == nullptr) {
        if (g_renderReady) {
            if (gpu != nullptr) gpu->DeviceLost();
            g_graphicsContext->ShutdownSurface();
            g_renderReady = false;
        }
        return JNI_TRUE;
    }
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) return JNI_FALSE;
    const bool ok = AttachSurface(window, width, height);
    ANativeWindow_release(window);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeRunFrame(JNIEnv *, jclass) {
    RunFrame();
    return 0;
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetPadButtons(JNIEnv *, jclass, jint port, jint buttons) {
    if (port < 0 || port > 1) return;
    __CtrlUpdateButtons(static_cast<u32>(buttons), 0);
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetPadAnalog(JNIEnv *, jclass, jint port, jint lx,
                                                            jint ly, jint rx, jint ry) {
    if (port != 0) return;
    __CtrlSetAnalogXY(CTRL_STICK_LEFT, lx / 255.0f, ly / 255.0f);
    __CtrlSetAnalogXY(CTRL_STICK_RIGHT, rx / 255.0f, ry / 255.0f);
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeShutdown(JNIEnv *, jclass) {
    ShutdownCore();
}

}  // extern "C"

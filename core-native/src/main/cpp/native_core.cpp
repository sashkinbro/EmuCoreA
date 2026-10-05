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
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/CPUDetect.h"
#include "Common/Crypto/md5.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/File/VFS/VFS.h"
#include "Common/File/VFS/ZipFileReader.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/OpenGL/OpenGLGraphicsContext.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanGraphicsContext.h"
#include "Common/Log.h"
#include "Common/Log/LogManager.h"
#include "Common/Serialize/Serializer.h"
#include "Common/System/Display.h"
#include "Common/System/System.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/TimeUtil.h"

#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreParameter.h"
#include "Core/CoreTiming.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/FileSystems/BlockDevices.h"
#include "Core/FileSystems/ISOFileSystem.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceDisplay.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HW/StereoResampler.h"
#include "Core/Loaders.h"
#include "Core/MemMap.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "GPU/GPUCommon.h"

#include "Core/FrameTiming.h"
#include "Core/HLE/sceUtility.h"

#include "android/jni/AndroidAudio.h"
#include "fd_file_loader.h"
#include "native_vulkan_presentation.h"
#include "shader_chain.h"

#define LOG_TAG "EmuCoreA-Native"
#define NLOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define NLOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define NLOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Implemented by achievements_bridge.cpp (compiled into the same library).
extern "C" void EmuCoreAAchievementsSetJavaVm(JavaVM *vm);
extern "C" void EmuCoreAAchievementsInitializeJava(JNIEnv *env);
extern "C" void EmuCoreAAchievementsOnFrame();

namespace {

JavaVM *g_vm = nullptr;
// Serializes RunFrame against surface attach/detach and shutdown, so closing
// the activity or turning the screen off cannot tear the swapchain down while
// a frame is being recorded or presented.
std::mutex g_nativeFrameMutex;
GraphicsContext *g_graphicsContext = nullptr;
StereoResampler g_resampler;
AndroidAudioState *g_audioState = nullptr;
bool g_audioStarted = false;
bool g_booted = false;
bool g_pendingBoot = false;
bool g_renderReady = false;
// Boot requests are queued and executed on the frame thread: that is the only
// thread where the OpenGL context exists, and it also serializes PSP_InitStart
// against a running frame loop.
bool g_bootRequested = false;
std::string g_bootRequestPath;
FileLoader *g_bootRequestLoader = nullptr;
// Cheat .ini requested by the frontend, applied once the disc ID is known.
std::string g_pendingCheatFile;
// Rewind history must be dropped on the frame thread (ring buffer + compressor).
bool g_clearRewindRequested = false;
// Set when the emulated game exits itself; reported to the frontend once.
bool g_corePoweredDown = false;
// Debug-only speed diagnostics independent of the frontend overlay.
int64_t g_diagLastWallUs = 0;
int64_t g_diagLastEmuUs = 0;
int g_diagFrames = 0;
// Frontend shader selection, stored for a future presentation hook.
int g_shaderEffect = 0;
std::string g_shaderPreset;
// Renderer requested by the frontend. 0 = software (unsupported), 1 = Vulkan,
// 2 = OpenGL ES. Stored before nativeInit so the graphics context is created
// for the right API; g_activeRenderer records what was actually created.
constexpr int kRendererSoftware = 0;
constexpr int kRendererVulkan = 1;
constexpr int kRendererOpenGL = 2;
std::atomic<int> g_requestedRenderer{kRendererVulkan};
int g_activeRenderer = kRendererVulkan;
std::string g_bootError;
int g_displayWidth = 1080;
int g_displayHeight = 1920;
float g_displayRefreshRate = 60.0f;
// g_Config has no constructor defaults; without Config::Init()/RestoreDefaults()
// every scalar is zero (iCpuCore = INTERPRETER, bVSync = false, ...), which made
// the core run at interpreter speed. Initialized once in nativeInit().
bool g_configReady = false;

// The currently attached Android window and its size. Kept so an idle renderer
// switch (before a boot or between sessions) can rebuild the graphics context
// and re-attach the same surface without a round trip through Kotlin.
ANativeWindow *g_surfaceWindow = nullptr;
int g_surfaceWidth = 0;
int g_surfaceHeight = 0;

// Native EGL state for the OpenGL ES renderer. PPSSPP's Java app lets
// GLSurfaceView own the EGL context; this frontend creates and owns it itself,
// always on the emulation/frame thread.
ANativeWindow *g_glWindow = nullptr;
EGLDisplay g_eglDisplay = EGL_NO_DISPLAY;
EGLConfig g_eglConfig = nullptr;
EGLContext g_eglContext = EGL_NO_CONTEXT;
EGLSurface g_eglSurface = EGL_NO_SURFACE;
bool g_glInitialized = false;

// PPSSPP's GL backend executes queued GPU commands on a dedicated render
// thread and the CPU thread may block on synchronous readbacks; running both
// on one thread deadlocks. The render thread owns the EGL context from
// creation until shutdown and swaps the window surface per presented frame.
std::thread g_glRenderThread;
std::mutex g_glStateMutex;
std::condition_variable g_glStateCv;
bool g_glInitDone = false;
bool g_glInitOk = false;

// Digits of the frontend-configurable MAC address / adhoc server IP. The
// frontend (libretro option layout) sends one hex digit per setting; these
// accumulate into the real string settings.
char g_macDigits[12] = {0};
bool g_macDigitsValid = false;
int g_adhocDigits[12] = {0};
bool g_adhocDigitsValid = false;
bool g_adhocUseIpAddress = false;

std::string ToString(JNIEnv *env, jstring value) {
    if (value == nullptr) return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars != nullptr ? chars : "";
    if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
    return out;
}

bool EqualsIgnoreCase(const std::string &a, const char *b) {
    const size_t len = strlen(b);
    if (a.size() != len) return false;
    for (size_t i = 0; i < len; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Accepts the enabled/disabled convention of the option catalogue plus the
// usual true/false, 1/0, on/off spellings.
bool ParseBool(const std::string &value, bool *out) {
    if (EqualsIgnoreCase(value, "enabled") || EqualsIgnoreCase(value, "true") ||
        EqualsIgnoreCase(value, "on") || EqualsIgnoreCase(value, "yes") ||
        EqualsIgnoreCase(value, "1")) {
        *out = true;
        return true;
    }
    if (EqualsIgnoreCase(value, "disabled") || EqualsIgnoreCase(value, "false") ||
        EqualsIgnoreCase(value, "off") || EqualsIgnoreCase(value, "no") ||
        EqualsIgnoreCase(value, "0")) {
        *out = false;
        return true;
    }
    return false;
}

// Mirrors Config::PostLoadCleanup() (private) for the fields this frontend
// depends on, then restores the shipped defaults. The frontend forwards every
// option afterwards, so the on-disk ppsspp.ini is deliberately never loaded:
// EmuCoreA's own stores are the single source of truth.
void InitializeConfigIfNeeded() {
    if (g_configReady) return;
    g_Config.Init();
    g_Config.RestoreDefaults(RestoreSettingsBits::SETTINGS, false);
    if (g_Config.iTexScalingLevel <= 0) g_Config.iTexScalingLevel = 1;
    g_Config.iAnisotropyLevel = std::clamp(g_Config.iAnisotropyLevel, 0, 4);
    g_Config.iInflightFrames = std::clamp(g_Config.iInflightFrames, 1, 2);
    if (g_Config.bAutoFrameSkip && g_Config.bSkipBufferEffects) g_Config.bSkipBufferEffects = false;
    if (g_Config.sMACAddress.length() != 17) g_Config.sMACAddress = CreateRandMAC();
    g_configReady = true;
    NLOGI("Config defaults restored (cpu=%d vsync=%d inflight=%d lang=%d model=%d)",
          g_Config.iCpuCore, g_Config.bVSync ? 1 : 0, g_Config.iInflightFrames, g_Config.iLanguage,
          g_Config.iPSPModel);
}

void EnsureMacDigits() {
    if (g_macDigitsValid) return;
    g_macDigitsValid = true;
    int out = 0;
    for (char ch : g_Config.sMACAddress) {
        if (ch == ':') continue;
        if (out >= 12) break;
        g_macDigits[out++] = ch;
    }
}

void RebuildMacAddress() {
    std::string mac;
    for (int i = 0; i < 12; ++i) {
        if (i > 0 && i % 2 == 0) mac += ':';
        mac += g_macDigits[i] != '\0' ? g_macDigits[i] : '0';
    }
    g_Config.sMACAddress = mac;
}

void EnsureAdhocDigits() {
    if (g_adhocDigitsValid) return;
    g_adhocDigitsValid = true;
    for (int i = 0; i < 12; ++i) g_adhocDigits[i] = 0;
}

// Mirrors the libretro option reconstruction: 12 decimal digits split into
// four dotted octets, with leading zeros in each octet suppressed.
void RebuildAdhocIp() {
    std::string address;
    bool leadingZero = true;
    for (int i = 0; i < 12; ++i) {
        if (i > 0 && i % 3 == 0) {
            address += '.';
            leadingZero = true;
        }
        const int digit = g_adhocDigits[i];
        if (digit != 0 || i % 3 == 2) leadingZero = false;
        if (!leadingZero) address += static_cast<char>('0' + std::clamp(digit, 0, 9));
    }
    g_Config.sProAdhocServer = address;
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

// ---------------------------------------------------------------------------
// RetroAchievements support. The achievements bridge (same shared library)
// reads guest RAM through these helpers and asks the core to hash a game the
// same way PPSSPP's own achievements integration does.
// ---------------------------------------------------------------------------
extern "C" void *EmuCoreANativeMemoryPointer() {
    return reinterpret_cast<void *>(Memory::GetPointerWriteUnchecked(PSP_GetKernelMemoryBase()));
}

extern "C" size_t EmuCoreANativeMemorySize() {
    return static_cast<size_t>(Memory::g_MemorySize);
}

namespace {

std::string FormatMd5Hash(const u8 digest[16]) {
    char hash[33];
    snprintf(hash, sizeof(hash), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7],
             digest[8], digest[9], digest[10], digest[11], digest[12], digest[13], digest[14], digest[15]);
    return std::string(hash);
}

bool HashIsoMember(ISOFileSystem *fs, const std::string &member, md5_context *md5) {
    const int handle = fs->OpenFile(member, FILEACCESS_READ);
    if (handle < 0) return false;
    const uint32_t size = static_cast<uint32_t>(fs->SeekFile(handle, 0, FILEMOVE_END));
    fs->SeekFile(handle, 0, FILEMOVE_BEGIN);
    if (size == 0) {
        fs->CloseFile(handle);
        return false;
    }
    auto buffer = std::make_unique<u8[]>(size);
    const bool ok = fs->ReadFile(handle, buffer.get(), size) == size;
    fs->CloseFile(handle);
    if (!ok) return false;
    ppsspp_md5_update(md5, buffer.get(), static_cast<int>(size));
    return true;
}

}  // namespace

JNIEnv *getEnv();

namespace {

constexpr const char *kSafPrefix = "/__emucorea_saf__/";

// Opens an EmuCoreA SAF VFS path by asking the Java PspStorageBridge for a raw
// descriptor, then wrapping it in the same FdFileLoader used for booting. No
// image bytes are copied anywhere: reads go straight through pread64.
FileLoader *ConstructSafFileLoader(const std::string &path) {
    JNIEnv *env = getEnv();
    if (env == nullptr) return nullptr;
    jclass cls = env->FindClass("com/sbro/emucorea/core/PspStorageBridge");
    if (cls == nullptr) {
        env->ExceptionClear();
        return nullptr;
    }
    jmethodID open = env->GetStaticMethodID(cls, "open", "(Ljava/lang/String;)I");
    if (open == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        return nullptr;
    }
    jstring jpath = env->NewStringUTF(path.c_str());
    const jint fd = env->CallStaticIntMethod(cls, open, jpath);
    env->DeleteLocalRef(jpath);
    env->DeleteLocalRef(cls);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    if (fd < 0) return nullptr;
    auto *loader = new FdFileLoader(fd, path);
    close(fd);  // FdFileLoader keeps its own duplicate.
    return loader;
}

}  // namespace

extern "C" bool EmuCoreANativeAchievementHash(const char *path, char *hash) {
    if (path == nullptr || *path == '\0' || hash == nullptr) return false;

    FileLoader *loader = nullptr;
    if (strncmp(path, kSafPrefix, strlen(kSafPrefix)) == 0) {
        loader = ConstructSafFileLoader(path);
    } else {
        loader = ConstructFileLoader(Path(path));
    }
    if (loader == nullptr) return false;
    std::string error;
    IdentifiedFileType fileType;
    loader = ResolveFileLoaderTarget(loader, &fileType, &error);

    std::string digest;
    switch (fileType) {
    case IdentifiedFileType::PSP_ISO:
    case IdentifiedFileType::PSP_ISO_NP: {
        std::shared_ptr<BlockDevice> blockDevice(ConstructBlockDevice(loader, &error));
        if (blockDevice) {
            md5_context md5;
            ppsspp_md5_starts(&md5);
            SequentialHandleAllocator alloc;
            auto fs = std::make_unique<ISOFileSystem>(&alloc, blockDevice);
            if (HashIsoMember(fs.get(), "PSP_GAME/PARAM.SFO", &md5) &&
                HashIsoMember(fs.get(), "PSP_GAME/SYSDIR/EBOOT.BIN", &md5)) {
                u8 out[16];
                ppsspp_md5_finish(&md5, out);
                digest = FormatMd5Hash(out);
            }
        }
        break;
    }
    case IdentifiedFileType::PSP_PBP:
    case IdentifiedFileType::PSP_PBP_DIRECTORY:
    case IdentifiedFileType::PSP_ELF: {
        md5_context md5;
        ppsspp_md5_starts(&md5);
        const size_t fileSize = static_cast<size_t>(std::min((s64)(1024 * 1024 * 64), loader->FileSize()));
        std::vector<u8> buffer(fileSize);
        loader->ReadAt(0, fileSize, buffer.data(), FileLoader::Flags::NONE);
        ppsspp_md5_update(&md5, buffer.data(), static_cast<int>(buffer.size()));
        u8 out[16];
        ppsspp_md5_finish(&md5, out);
        digest = FormatMd5Hash(out);
        break;
    }
    default:
        NLOGW("Unsupported file type for RetroAchievements hashing: %s", path);
        break;
    }

    delete loader;
    if (digest.empty()) return false;
    std::memcpy(hash, digest.c_str(), 32);
    hash[32] = '\0';
    return true;
}

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

void EmuCoreA_AttachThreadToJNI() {
    JNIEnv *env = nullptr;
    if (g_vm != nullptr && g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_vm->AttachCurrentThread(&env, nullptr);
    }
}

void EmuCoreA_DetachThreadFromJNI() {
    if (g_vm != nullptr) {
        JNIEnv *env = nullptr;
        if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) {
            g_vm->DetachCurrentThread();
        }
    }
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
    case SYSPROP_DEVICE_TYPE:
        return DEVICE_TYPE_MOBILE;
    case SYSPROP_DISPLAY_XRES:
        return g_display.pixel_xres > 0 ? g_display.pixel_xres : g_displayWidth;
    case SYSPROP_DISPLAY_YRES:
        return g_display.pixel_yres > 0 ? g_display.pixel_yres : g_displayHeight;
    case SYSPROP_DISPLAY_DPI:
        return 240;
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
    case SYSPROP_IS_HEADLESS:
    case SYSPROP_ANDROID_SCOPED_STORAGE:
    case SYSPROP_SUPPORTS_PERMISSIONS:
    case SYSPROP_HAS_TEXT_INPUT_DIALOG:
    case SYSPROP_SUPPORTS_SUSTAINED_PERF_MODE:
        return false;
    default:
        return false;
    }
}

std::string System_GetProperty(SystemProperty prop) {
    switch (prop) {
    case SYSPROP_NAME:
        return "EmuCoreA";
    case SYSPROP_BOARDNAME:
        return System_GetProperty(SYSPROP_NAME);
    default:
        return "";
    }
}
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

const char *RendererName(int renderer) {
    switch (renderer) {
    case kRendererSoftware: return "Software";
    case kRendererOpenGL: return "OpenGL";
    case kRendererVulkan: return "Vulkan";
    default: return "Unknown";
    }
}

// Creates the graphics context for the renderer stored by nativeSetRenderer.
// Must run before PSP_Init, because BootGame picks the GPU core from it.
bool CreateGraphicsContext() {
    if (g_graphicsContext != nullptr) return true;

    const int requested = g_requestedRenderer.load();
    if (requested == kRendererOpenGL) {
        g_activeRenderer = kRendererOpenGL;
        // PPSSPP's GL backend renders through its render manager queue; the
        // EGL context is created on the frame thread in InitGLOnFrameThread()
        // once a surface exists. InitAPI is a no-op for this context type.
        g_graphicsContext = new OpenGLGraphicsContext();
        Core_SetGraphicsContext(g_graphicsContext);
        SetGPUBackend(GPUBackend::OPENGL);
        NLOGI("OpenGL ES renderer selected");
        return true;
    }
    if (requested == kRendererSoftware) {
        NLOGW("Software rendering is not supported by the PSP core; using Vulkan instead");
    }
    g_activeRenderer = kRendererVulkan;

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
    NLOGI("%s renderer initialized (Vulkan API: %s)", RendererName(g_activeRenderer),
          deviceName.c_str());
    return true;
}

// Creates the EGL display/config/context shared by all sessions. Runs on the
// frame thread, which owns the context from then on.
bool CreateEGLContext() {
    if (g_eglDisplay != EGL_NO_DISPLAY && g_eglContext != EGL_NO_CONTEXT) return true;

    g_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_eglDisplay == EGL_NO_DISPLAY) {
        NLOGE("eglGetDisplay failed");
        return false;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(g_eglDisplay, &major, &minor)) {
        NLOGE("eglInitialize failed: 0x%x", eglGetError());
        g_eglDisplay = EGL_NO_DISPLAY;
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLint numConfigs = 0;
    if (!eglChooseConfig(g_eglDisplay, configAttribs, &g_eglConfig, 1, &numConfigs) || numConfigs == 0) {
        const EGLint fallbackAttribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_NONE
        };
        if (!eglChooseConfig(g_eglDisplay, fallbackAttribs, &g_eglConfig, 1, &numConfigs) ||
            numConfigs == 0) {
            NLOGE("eglChooseConfig failed: 0x%x", eglGetError());
            return false;
        }
    }

    // PPSSPP prefers a GLES3 context but still supports GLES2 devices; try 3
    // first and fall back like the standalone app does.
    const EGLint contextAttribs3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g_eglContext = eglCreateContext(g_eglDisplay, g_eglConfig, EGL_NO_CONTEXT, contextAttribs3);
    if (g_eglContext == EGL_NO_CONTEXT) {
        const EGLint contextAttribs2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        g_eglContext = eglCreateContext(g_eglDisplay, g_eglConfig, EGL_NO_CONTEXT, contextAttribs2);
    }
    if (g_eglContext == EGL_NO_CONTEXT) {
        NLOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }
    NLOGI("EGL %d.%d context created", (int)major, (int)minor);
    return true;
}

// The render thread's whole life: acquire the EGL context, build the draw
// context/render manager, then drain the command queue (which also performs
// the buffer swap) until the emu side asks us to exit.
void GLRenderThreadMain() {
    SetCurrentThreadName("EmuCoreA-GLRender");

    if (!eglMakeCurrent(g_eglDisplay, g_eglSurface, g_eglSurface, g_eglContext)) {
        NLOGE("eglMakeCurrent failed on the render thread: 0x%x", eglGetError());
        std::lock_guard<std::mutex> lock(g_glStateMutex);
        g_glInitDone = true;
        g_glInitOk = false;
        g_glStateCv.notify_all();
        return;
    }

    // InitSurface creates the draw context and the render manager, so it must
    // run before ThreadStart (which creates the manager's device objects).
    std::string error;
    if (!g_graphicsContext->InitSurface(WINDOWSYSTEM_ANDROID, nullptr, nullptr, &error)) {
        NLOGE("OpenGL InitSurface failed: %s", error.c_str());
        std::lock_guard<std::mutex> lock(g_glStateMutex);
        g_glInitDone = true;
        g_glInitOk = false;
        g_glStateCv.notify_all();
        eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return;
    }

    Draw::DrawContext *draw = g_graphicsContext->GetDrawContext();
    auto *renderManager =
        draw != nullptr
            ? reinterpret_cast<GLRenderManager *>(draw->GetNativeObject(Draw::NativeObject::RENDER_MANAGER))
            : nullptr;
    if (renderManager != nullptr) {
        // A self-owned EGL surface swaps here (the Java app lets GLSurfaceView
        // do it). The swap is deliberately not vsynced: the core already paces
        // frames in FrameTiming::PostSubmit, and double throttling halves the
        // frame rate (30 fps instead of 60) on 60 Hz panels.
        renderManager->SetSwapIntervalFunction([](int) {
            if (g_eglDisplay != EGL_NO_DISPLAY) eglSwapInterval(g_eglDisplay, 0);
        });
        renderManager->SetSwapFunction([]() {
            if (g_eglDisplay != EGL_NO_DISPLAY && g_eglSurface != EGL_NO_SURFACE) {
                eglSwapBuffers(g_eglDisplay, g_eglSurface);
            }
        });
    }

    g_graphicsContext->ThreadStart();

    {
        std::lock_guard<std::mutex> lock(g_glStateMutex);
        g_glInitDone = true;
        g_glInitOk = true;
        g_glStateCv.notify_all();
    }

    // Blocks inside until a full frame (with a swap) was processed, or until
    // NotifyEmuThreadExit queues the exit task.
    while (g_graphicsContext->ThreadFrame()) {
    }
    g_graphicsContext->ThreadEnd();
    eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    NLOGI("OpenGL render thread finished");
}

// Brings up the OpenGL backend. Runs on the frame thread and blocks until the
// render thread has created the draw context, so the boot path can rely on it.
bool InitGLOnFrameThread() {
    if (g_glInitialized) return true;
    if (g_glWindow == nullptr) return false;
    if (!CreateEGLContext()) return false;

    if (g_eglSurface == EGL_NO_SURFACE) {
        g_eglSurface = eglCreateWindowSurface(g_eglDisplay, g_eglConfig, g_glWindow, nullptr);
        if (g_eglSurface == EGL_NO_SURFACE) {
            NLOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_glStateMutex);
        g_glInitDone = false;
        g_glInitOk = false;
    }
    g_glRenderThread = std::thread(GLRenderThreadMain);
    {
        std::unique_lock<std::mutex> lock(g_glStateMutex);
        g_glStateCv.wait(lock, [] { return g_glInitDone; });
    }
    if (!g_glInitOk) {
        if (g_glRenderThread.joinable()) g_glRenderThread.join();
        return false;
    }

    g_glInitialized = true;
    NLOGI("OpenGL ES surface ready");
    return true;
}

void ShutdownGLOnFrameThread() {
    if (!g_glInitialized) return;
    // Wakes the render thread out of ThreadFrame() and lets it run ThreadEnd.
    g_graphicsContext->NotifyEmuThreadExit();
    if (g_glRenderThread.joinable()) g_glRenderThread.join();
    g_graphicsContext->ShutdownSurface();
    g_glInitialized = false;
}

// Releases the EGL surface/context/display. Safe to call when GL was never
// brought up.
void DestroyEGL() {
    if (g_eglDisplay == EGL_NO_DISPLAY) return;
    eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g_eglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(g_eglDisplay, g_eglSurface);
        g_eglSurface = EGL_NO_SURFACE;
    }
    if (g_eglContext != EGL_NO_CONTEXT) {
        eglDestroyContext(g_eglDisplay, g_eglContext);
        g_eglContext = EGL_NO_CONTEXT;
    }
    eglTerminate(g_eglDisplay);
    eglReleaseThread();
    g_eglDisplay = EGL_NO_DISPLAY;
    g_eglConfig = nullptr;
}

void DestroyEGLSurface() {
    if (g_eglDisplay == EGL_NO_DISPLAY || g_eglSurface == EGL_NO_SURFACE) return;
    eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(g_eglDisplay, g_eglSurface);
    g_eglSurface = EGL_NO_SURFACE;
}

// Takes ownership of preOpenedLoader: on success the core deletes it during
// CPU_Shutdown, on failure it is deleted here so a failed boot cannot leak the
// (possibly pre-opened) loader.
bool BootGame(const std::string &gamePath, FileLoader *preOpenedLoader) {
    if (g_graphicsContext == nullptr) {
        delete preOpenedLoader;
        return false;
    }

    CoreParameter coreParam{};
    coreParam.enableSound = true;
    coreParam.fileToStart = Path(gamePath);
    coreParam.fileLoader = preOpenedLoader;
    if (preOpenedLoader != nullptr && coreParam.fileToStart.empty()) {
        coreParam.fileToStart = preOpenedLoader->GetPath();
    }
    coreParam.startBreak = false;
    coreParam.headLess = false;
    coreParam.graphicsContext = g_graphicsContext;
    coreParam.gpuCore = g_activeRenderer == kRendererOpenGL ? GPUCORE_GLES : GPUCORE_VULKAN;
    coreParam.cpuCore = CPUCore::JIT;
    coreParam.bUseVertexDecoderJit = true;

    // Output/display size drives the presentation viewport. Without it
    // (zero-initialized CoreParameter) every frame is drawn into a 0x0
    // viewport, which presents as a black screen. Mirrors EmuScreen::bootGame.
    const int displayWidth = g_display.pixel_xres > 0 ? g_display.pixel_xres : g_displayWidth;
    const int displayHeight = g_display.pixel_yres > 0 ? g_display.pixel_yres : g_displayHeight;
    coreParam.pixelWidth = displayWidth;
    coreParam.pixelHeight = displayHeight;
    if (g_Config.iInternalResolution == 0) {
        coreParam.renderWidth = displayWidth;
        coreParam.renderHeight = displayHeight;
    } else {
        coreParam.renderWidth = 480 * g_Config.iInternalResolution;
        coreParam.renderHeight = 272 * g_Config.iInternalResolution;
    }
    NLOGI("Display %dx%d render %dx%d", coreParam.pixelWidth, coreParam.pixelHeight,
          coreParam.renderWidth, coreParam.renderHeight);

    // Asynchronous boot, exactly like the libretro wrapper: the loader thread
    // runs in the background and PSP_InitUpdate is polled from the frame loop
    // once the surface (and with it the draw context) is ready.
    if (!PSP_InitStart(coreParam)) {
        NLOGE("PSP_InitStart failed: %s", coreParam.errorString.c_str());
        // PSP_InitStart only fails before it adopts the parameter, so the
        // loader is still ours to release.
        delete preOpenedLoader;
        return false;
    }
    g_bootError.clear();
    g_pendingBoot = true;
    NLOGI("Game boot started: %s", coreParam.fileToStart.c_str());
    return true;
}

bool AttachSurface(ANativeWindow *window, int width, int height) {
    if (g_graphicsContext == nullptr) return false;

    if (g_activeRenderer == kRendererOpenGL) {
        // The GL surface is created on the frame thread (the thread that owns
        // the EGL context). Take a reference to the window so it stays valid
        // until the frame loop tears the EGL surface down.
        ANativeWindow_acquire(window);
        if (g_glWindow != nullptr) ANativeWindow_release(g_glWindow);
        g_glWindow = window;
        if (g_glInitialized) {
            // Re-attach without an explicit detach: tear the old window
            // surface down so the frame thread rebuilds it with the new one.
            ShutdownGLOnFrameThread();
            DestroyEGLSurface();
        }
    } else {
        std::string error;
        if (!g_graphicsContext->InitSurface(WINDOWSYSTEM_ANDROID, window, nullptr, &error)) {
            NLOGE("InitSurface failed: %s", error.c_str());
            return false;
        }
        // InitSurface() already created the swapchain, the draw context, its
        // presets and the backbuffer event. Calling any of those again starts
        // the render threads twice, which terminates the process.
        if (g_graphicsContext->GetDrawContext() == nullptr) return false;
    }

    // Publish the window size so display-dependent code (presentation viewport,
    // DPI heuristics) has real dimensions. PPSSPP's own Android frontend does
    // this from SizeManager; without it the presentation draws into a 0x0
    // viewport and presents black frames.
    if (width > 0 && height > 0) {
        g_display.Recalculate(width, height, 1.0f, 1.0f, 1.0f);
        g_display.display_hz = g_displayRefreshRate;
        PSP_CoreParameter().pixelWidth = width;
        PSP_CoreParameter().pixelHeight = height;
    }

    // A live game that just got its surface back needs its GPU objects
    // restored against the new draw context (framebuffers, pipelines, and the
    // presentation all cache it).
    if (gpu != nullptr && g_booted) {
        gpu->DeviceRestore(g_graphicsContext->GetDrawContext());
    }

#if defined(EMUCOREA_HAVE_LIBRASHADER)
    // The shader-chain presentation is strictly opt-in: with no preset enabled
    // the core must use PPSSPP's untouched real-swapchain path. Installing it
    // unconditionally changed behavior even in pass-through mode. It only
    // exists for Vulkan; the GL backend ignores it.
    if (g_activeRenderer == kRendererVulkan) {
        auto *vulkan = static_cast<VulkanContext *>(g_graphicsContext->GetAPIContext());
        if (vulkan != nullptr && vulkan->GetPresentation() == nullptr && emucorer::shader_chain::IsEnabled()) {
            auto presentation = std::make_unique<NativeVulkanPresentation>(vulkan);
            if (presentation->Create(vulkan)) {
                vulkan->SetPresentation(std::move(presentation));
                NLOGI("Shader chain presentation installed");
            } else {
                NLOGW("Shader chain presentation unavailable; using the direct swapchain path");
            }
        }
    }
#endif

    g_renderReady = true;
    NLOGI("Surface attached %dx%d (%s)", width, height, RendererName(g_activeRenderer));
    return true;
}

// Copies the frontend's CWCheat file into the core's cheat directory under
// the booted game's disc ID and asks the cheat engine to reload it. Called
// with g_nativeFrameMutex held; retries every frame until the disc ID exists.
void ApplyPendingCheatsLocked() {
    if (g_pendingCheatFile.empty() || !g_booted) return;

    const std::string discID = g_paramSFO.GetDiscID();
    if (discID.empty()) return;

    FILE *in = File::OpenCFile(Path(g_pendingCheatFile), "rb");
    if (in == nullptr) {
        NLOGW("Unable to open cheat file: %s", g_pendingCheatFile.c_str());
        g_pendingCheatFile.clear();
        return;
    }
    std::vector<u8> data;
    std::array<u8, 16384> buffer;
    for (;;) {
        const size_t n = fread(buffer.data(), 1, buffer.size(), in);
        data.insert(data.end(), buffer.begin(), buffer.begin() + n);
        if (n < buffer.size()) break;
    }
    fclose(in);

    const Path cheatDirectory = GetSysDirectory(DIRECTORY_CHEATS);
    File::CreateFullPath(cheatDirectory);
    const Path target = cheatDirectory / (discID + ".ini");
    FILE *out = File::OpenCFile(target, "wb");
    if (out == nullptr) {
        NLOGE("Unable to write cheat file: %s", target.c_str());
        g_pendingCheatFile.clear();
        return;
    }
    const size_t written = fwrite(data.data(), 1, data.size(), out);
    fclose(out);
    if (written != data.size()) {
        NLOGE("Short write on cheat file: %s", target.c_str());
        g_pendingCheatFile.clear();
        return;
    }

    g_Config.bEnableCheats = true;
    g_Config.bReloadCheats = true;
    NLOGI("Cheats applied for %s (%zu bytes)", discID.c_str(), data.size());
    g_pendingCheatFile.clear();
}

void RunFrame() {
    std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
    if (g_graphicsContext == nullptr || !g_renderReady) return;

    // The OpenGL context lives on this thread, so it is created on the first
    // frame after the surface arrives.
    if (g_activeRenderer == kRendererOpenGL && !g_glInitialized) {
        if (!InitGLOnFrameThread()) return;
    }

    if (g_bootRequested) {
        g_bootRequested = false;
        const std::string path = g_bootRequestPath;
        FileLoader *loader = g_bootRequestLoader;
        g_bootRequestLoader = nullptr;
        if (!BootGame(path, loader)) {
            NLOGE("Queued boot failed: %s", path.c_str());
        }
    }

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
            // EmuScreen does this when the asynchronous boot finishes; without
            // it the core stays in CORE_POWERDOWN and PSP_RunLoopWhileState()
            // returns immediately, which presents a black screen forever.
            coreState = PSP_CoreParameter().startBreak ? CORE_STEPPING_CPU : CORE_RUNNING_CPU;
            // Tells the core it is actually in-game: RetroAchievements gating,
            // savedata, screenshots and the websocket subscribers all check it.
            UpdateUIState(UISTATE_INGAME);
            System_Notify(SystemNotification::BOOT_DONE);
            break;
        case BootState::Off:
        default:
            return;
        }
    }

    if (!g_booted || gpu == nullptr) return;

    ApplyPendingCheatsLocked();

    Draw::DrawContext *draw = g_graphicsContext->GetDrawContext();
    if (draw == nullptr) return;

    Core_StateProcessed();
    draw->BeginFrame(Draw::DebugFlags::NONE);

    const DeviceOrientation orientation = g_display.GetDeviceOrientation();
    const DisplayLayoutConfig &layout = g_Config.GetDisplayLayoutConfig(orientation);
    __DisplaySetDisplayLayoutConfig(layout);
    const Draw::Viewport viewport{0.0f, 0.0f, (float)g_display.pixel_xres,
                                  (float)g_display.pixel_yres, 0.0f, 1.0f};

    // Decides Immediate/Mailbox/FIFO from g_Config.bVSync/bLowLatencyPresent.
    // The core defers its 60 Hz wait here and flushes it in PostSubmit below.
    g_frameTiming.ComputePresentMode(draw, false);

    PSP_UpdateDebugStats(g_Config.bLogFrameDrops);

    SaveState::Process();

    gpu->BeginHostFrame(layout);
    PSP_RunLoopWhileState();
    switch (coreState) {
    case CORE_NEXTFRAME:
        // Reached the end of the frame while running at full blast.
        coreState = CORE_RUNNING_CPU;
        break;
    case CORE_POWERDOWN:
        // The game called sceKernelExitGame (or similar). EmuScreen mirrors
        // this by switching back to the menu; do the equivalent at the end of
        // this frame so the loop stops running unpaced over a dead core.
        g_corePoweredDown = true;
        break;
    default:
        break;
    }

    // Must run before EndHostFrame: it records the post-processing/copy passes
    // while the previous frame's render state is still valid.
    gpu->PrepareCopyDisplayToOutput(layout);
    gpu->EndHostFrame();

    using namespace Draw;
    draw->BindFramebufferAsRenderTarget(nullptr, {RPAction::CLEAR, RPAction::CLEAR, RPAction::CLEAR}, "BackBuffer");
    gpu->CopyDisplayToOutput(layout);
    // CopyDisplayToOutput can leave a custom viewport behind; restore it for
    // the next frame and for the swapchain pass.
    draw->SetViewport(viewport);
    draw->SetScissorRect(0, 0, g_display.pixel_xres, g_display.pixel_yres);
    draw->EndFrame();

    g_frameTiming.PostSubmit();
    draw->Present(g_frameTiming.PresentMode());

    // The dedicated GL render thread executes the queued commands and swaps.

    g_graphicsContext->Poll();
    if (g_clearRewindRequested) {
        g_clearRewindRequested = false;
        SaveState::ClearRewind();
    }
    EmuCoreAAchievementsOnFrame();

    if (g_corePoweredDown) {
        g_booted = false;
        UpdateUIState(UISTATE_MENU);
        NLOGI("Core powered down by the game");
    }

#ifndef NDEBUG
    // Emulated time vs wall time: 100% means the game runs at full speed even
    // when the content itself is 30 fps (the frontend FPS counter cannot tell).
    // The baseline is only taken while the core is actually inited, so the
    // loader's clock reset cannot produce a garbage first window.
    if (PSP_IsInited()) {
        ++g_diagFrames;
        const int64_t nowWallUs = static_cast<int64_t>(time_now_d() * 1000000.0);
        if (g_diagLastWallUs == 0) {
            g_diagLastWallUs = nowWallUs;
            g_diagLastEmuUs = CoreTiming::GetGlobalTimeUs();
            g_diagFrames = 0;
        } else if (nowWallUs - g_diagLastWallUs >= 2'000'000) {
            const int64_t emuDeltaUs = CoreTiming::GetGlobalTimeUs() - g_diagLastEmuUs;
            const int64_t wallDeltaUs = nowWallUs - g_diagLastWallUs;
            if (emuDeltaUs >= 0 && emuDeltaUs <= wallDeltaUs * 3) {
                NLOGI("diag speed=%.1f%% frames=%d fps=%.1f %s", emuDeltaUs * 100.0 / wallDeltaUs,
                      g_diagFrames, g_diagFrames * 1e6 / wallDeltaUs, RendererName(g_activeRenderer));
            }
            g_diagLastWallUs = nowWallUs;
            g_diagLastEmuUs = CoreTiming::GetGlobalTimeUs();
            g_diagFrames = 0;
        }
    }
#endif
}

bool SaveStateToFile(const std::string &path) {
    std::vector<u8> data;
    if (SaveState::SaveToRam(data) != CChunkFileReader::ERROR_NONE) {
        NLOGE("SaveToRam failed");
        return false;
    }
    FILE *file = File::OpenCFile(Path(path), "wb");
    if (file == nullptr) return false;
    const size_t written = fwrite(data.data(), 1, data.size(), file);
    fclose(file);
    return written == data.size();
}

bool LoadStateFromFile(const std::string &path) {
    FILE *file = File::OpenCFile(Path(path), "rb");
    if (file == nullptr) return false;
    std::vector<u8> data;
    std::array<u8, 65536> buffer;
    for (;;) {
        const size_t n = fread(buffer.data(), 1, buffer.size(), file);
        data.insert(data.end(), buffer.begin(), buffer.begin() + n);
        if (n < buffer.size()) break;
    }
    fclose(file);
    std::string error;
    const bool ok = SaveState::LoadFromRam(data, &error) == CChunkFileReader::ERROR_NONE;
    if (!ok) NLOGE("LoadFromRam failed: %s", error.c_str());
    return ok;
}

void ShutdownCore() {
    std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
    StopAudio();
    if (g_audioState != nullptr) {
        AndroidAudio_Shutdown(g_audioState);
        g_audioState = nullptr;
    }
    if (g_bootRequestLoader != nullptr) {
        // A boot request that never reached the frame thread: the loader is
        // still owned here.
        delete g_bootRequestLoader;
        g_bootRequestLoader = nullptr;
    }
    g_bootRequested = false;
    g_bootRequestPath.clear();
    if (g_booted) {
        PSP_Shutdown(true);
        g_booted = false;
        UpdateUIState(UISTATE_MENU);
    } else if (g_pendingBoot) {
        // A boot request was started but never reached PSP_InitUpdate. Let the
        // loader thread finish so the core takes over (and later frees) the
        // pre-opened FileLoader, then tear the half-booted core down. Without
        // this the SAF descriptor's duplicate would stay open for the life of
        // the process.
        BootState state = PollBootState();
        while (state == BootState::Booting) {
            sleep_ms(5, "emucorea-shutdown-wait");
            state = PollBootState();
        }
        std::string bootError;
        PSP_InitUpdate(&bootError);
        g_pendingBoot = false;
        if (state == BootState::Complete) {
            PSP_Shutdown(true);
        }
    }
    if (g_graphicsContext != nullptr) {
        if (g_renderReady) {
            if (gpu != nullptr) gpu->DeviceLost();
            if (g_activeRenderer == kRendererOpenGL) {
                ShutdownGLOnFrameThread();
            } else {
                g_graphicsContext->ShutdownSurface();
            }
            g_renderReady = false;
        }
        g_graphicsContext->ShutdownAPI();
        delete g_graphicsContext;
        g_graphicsContext = nullptr;
    }
    DestroyEGL();
    if (g_glWindow != nullptr) {
        ANativeWindow_release(g_glWindow);
        g_glWindow = nullptr;
    }
    if (g_surfaceWindow != nullptr) {
        ANativeWindow_release(g_surfaceWindow);
        g_surfaceWindow = nullptr;
    }
    if (g_threadManager.IsInitialized()) {
        g_threadManager.Teardown();
    }
}

// Recomputes the CoreParameter render size from iInternalResolution. The
// resolution is captured into CoreParameter at boot and from there into the
// GPU; a mid-session change must push the new size into the live GPU as well.
// Mirrors NativeApp's handling of PPSSPP's own display settings.
void ApplyInternalResolution() {
    if (!PSP_IsInited() || gpu == nullptr) return;
    const int displayWidth = g_display.pixel_xres > 0 ? g_display.pixel_xres : g_displayWidth;
    const int displayHeight = g_display.pixel_yres > 0 ? g_display.pixel_yres : g_displayHeight;
    if (g_Config.iInternalResolution == 0) {
        PSP_CoreParameter().renderWidth = displayWidth;
        PSP_CoreParameter().renderHeight = displayHeight;
    } else {
        const int scale = std::clamp(g_Config.iInternalResolution, 1, 10);
        PSP_CoreParameter().renderWidth = 480 * scale;
        PSP_CoreParameter().renderHeight = 272 * scale;
    }
    const DisplayLayoutConfig &layout = g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation());
    gpu->NotifyRenderResized(layout);
    NLOGI("Render resolution now %dx%d", PSP_CoreParameter().renderWidth, PSP_CoreParameter().renderHeight);
}

void NotifyGpuConfigChanged() {
    if (PSP_IsInited() && gpu != nullptr) {
        gpu->NotifyConfigChanged();
    }
}

void NotifyGpuDisplayResized() {
    if (PSP_IsInited() && gpu != nullptr) {
        const DisplayLayoutConfig &layout = g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation());
        gpu->NotifyDisplayResized();
        (void)layout;
    }
}

// Numeric option helper that only accepts known values.
bool ParseIntInRange(const std::string &value, int low, int high, int *out) {
    char *end = nullptr;
    const long parsed = strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') return false;
    if (parsed < low || parsed > high) return false;
    *out = static_cast<int>(parsed);
    return true;
}

// Applies one native config key/value. Both the raw libretro option keys
// (ppsspp_*) and the short native keys used internally are accepted; the
// ppsspp keys follow exactly the semantics of the libretro core's
// check_variables() so the UI catalogue and the core cannot diverge.
void ApplyNativeConfig(const std::string &key, const std::string &value) {
    bool on = false;
    int number = 0;
    const bool hasBool = ParseBool(value, &on);

    // ----- Native short keys (legacy internal contract) -----
    if (key == "internal_resolution") {
        if (ParseIntInRange(value, 0, 10, &number)) {
            g_Config.iInternalResolution = number;
            if (PSP_IsInited()) ApplyInternalResolution();
        }
        return;
    }
    if (key == "frameskip") {
        if (ParseIntInRange(value, 0, 9, &number)) g_Config.iFrameSkip = number;
        return;
    }
    if (key == "auto_frameskip") {
        if (hasBool) g_Config.bAutoFrameSkip = on;
        return;
    }
    if (key == "frame_duplication") {
        if (hasBool) g_Config.bRenderDuplicateFrames = on;
        return;
    }
    if (key == "texture_filtering") {
        if (ParseIntInRange(value, 1, 4, &number)) g_Config.iTexFiltering = number;
        return;
    }
    if (key == "texture_scaling_level") {
        if (ParseIntInRange(value, 1, 5, &number)) g_Config.iTexScalingLevel = number;
        return;
    }
    if (key == "volume") {
        if (ParseIntInRange(value, 0, 100, &number)) g_Config.iGameVolume = number;
        return;
    }
    if (key == "skip_buffer_effects") {
        if (hasBool) g_Config.bSkipBufferEffects = on;
        return;
    }
    if (key == "fast_memory") {
        if (hasBool) g_Config.bFastMemory = on;
        return;
    }
    if (key == "cpu_core") {
        g_Config.iCpuCore = EqualsIgnoreCase(value, "jit") ? (int)CPUCore::JIT : (int)CPUCore::IR_INTERPRETER;
        return;
    }
    if (key == "crop16x9") {
        if (hasBool) g_Config.bDisplayCropTo16x9 = on;
        return;
    }
    if (key == "vsync") {
        if (hasBool) g_Config.bVSync = on;
        return;
    }
    if (key == "multi_threading") {
        if (hasBool) g_Config.bRenderMultiThreading = on;
        return;
    }

    // ----- System -----
    if (key == "ppsspp_cpu_core") {
        if (EqualsIgnoreCase(value, "JIT")) g_Config.iCpuCore = (int)CPUCore::JIT;
        else if (EqualsIgnoreCase(value, "IR JIT")) g_Config.iCpuCore = (int)CPUCore::IR_INTERPRETER;
        else if (EqualsIgnoreCase(value, "Interpreter")) g_Config.iCpuCore = (int)CPUCore::INTERPRETER;
        return;
    }
    if (key == "ppsspp_fast_memory") {
        if (hasBool) g_Config.bFastMemory = on;
        return;
    }
    if (key == "ppsspp_ignore_bad_memory_access") {
        if (hasBool) g_Config.bIgnoreBadMemAccess = on;
        return;
    }
    if (key == "ppsspp_io_timing_method") {
        if (EqualsIgnoreCase(value, "Fast")) g_Config.iIOTimingMethod = IOTIMING_FAST;
        else if (EqualsIgnoreCase(value, "Host")) g_Config.iIOTimingMethod = IOTIMING_HOST;
        else if (EqualsIgnoreCase(value, "Simulate UMD delays")) g_Config.iIOTimingMethod = IOTIMING_REALISTIC;
        else if (EqualsIgnoreCase(value, "Simulate UMD slow reading speed")) g_Config.iIOTimingMethod = IOTIMING_UMDSLOWREALISTIC;
        return;
    }
    if (key == "ppsspp_force_lag_sync") {
        if (hasBool) g_Config.bForceLagSync = on;
        return;
    }
    if (key == "ppsspp_locked_cpu_speed") {
        if (EqualsIgnoreCase(value, "disabled")) g_Config.iLockedCPUSpeed = 0;
        else if (ParseIntInRange(value, 1, 1000, &number)) g_Config.iLockedCPUSpeed = number;
        return;
    }
    if (key == "ppsspp_memstick_inserted") {
        if (hasBool) g_Config.bMemStickInserted = on;
        return;
    }
    if (key == "ppsspp_memstick_size") {
        if (ParseIntInRange(value, 1, 128, &number) && (number & (number - 1)) == 0) {
            g_Config.iMemStickSizeGB = number;
        }
        return;
    }
    if (key == "ppsspp_cache_iso") {
        if (hasBool) g_Config.bCacheFullIsoInRam = on;
        return;
    }
    if (key == "ppsspp_cheats") {
        if (hasBool) {
            g_Config.bEnableCheats = on;
            if (on) g_Config.bReloadCheats = true;
        }
        return;
    }
    if (key == "ppsspp_language") {
        if (EqualsIgnoreCase(value, "Automatic")) g_Config.iLanguage = -1;
        else if (EqualsIgnoreCase(value, "English")) g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
        else if (EqualsIgnoreCase(value, "Japanese")) g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_JAPANESE;
        else if (EqualsIgnoreCase(value, "French")) g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_FRENCH;
        else if (EqualsIgnoreCase(value, "Spanish")) g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_SPANISH;
        else if (EqualsIgnoreCase(value, "German")) g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_GERMAN;
        else if (EqualsIgnoreCase(value, "Italian")) g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_ITALIAN;
        else if (EqualsIgnoreCase(value, "Dutch")) g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_DUTCH;
        else if (EqualsIgnoreCase(value, "Portuguese")) g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_PORTUGUESE;
        else if (EqualsIgnoreCase(value, "Russian")) g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_RUSSIAN;
        else if (EqualsIgnoreCase(value, "Korean")) g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_KOREAN;
        else if (EqualsIgnoreCase(value, "Chinese Traditional")) g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_CHINESE_TRADITIONAL;
        else if (EqualsIgnoreCase(value, "Chinese Simplified")) g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_CHINESE_SIMPLIFIED;
        return;
    }
    if (key == "ppsspp_psp_model") {
        if (EqualsIgnoreCase(value, "psp_1000")) g_Config.iPSPModel = PSP_MODEL_FAT;
        else if (EqualsIgnoreCase(value, "psp_2000_3000")) g_Config.iPSPModel = PSP_MODEL_SLIM;
        return;
    }

    // ----- Video -----
    if (key == "ppsspp_internal_resolution") {
        if (EqualsIgnoreCase(value, "480x272")) g_Config.iInternalResolution = 1;
        else if (EqualsIgnoreCase(value, "960x544")) g_Config.iInternalResolution = 2;
        else if (EqualsIgnoreCase(value, "1440x816")) g_Config.iInternalResolution = 3;
        else if (EqualsIgnoreCase(value, "1920x1088")) g_Config.iInternalResolution = 4;
        else if (EqualsIgnoreCase(value, "2400x1360")) g_Config.iInternalResolution = 5;
        else if (EqualsIgnoreCase(value, "2880x1632")) g_Config.iInternalResolution = 6;
        else if (EqualsIgnoreCase(value, "3360x1904")) g_Config.iInternalResolution = 7;
        else if (EqualsIgnoreCase(value, "3840x2176")) g_Config.iInternalResolution = 8;
        else if (EqualsIgnoreCase(value, "4320x2448")) g_Config.iInternalResolution = 9;
        else if (EqualsIgnoreCase(value, "4800x2720")) g_Config.iInternalResolution = 10;
        else if (ParseIntInRange(value, 1, 10, &number)) g_Config.iInternalResolution = number;
        if (PSP_IsInited()) ApplyInternalResolution();
        return;
    }
    if (key == "ppsspp_mulitsample_level") {
        const int previous = g_Config.iMultiSampleLevel;
        if (EqualsIgnoreCase(value, "Disabled")) g_Config.iMultiSampleLevel = 0;
        else if (EqualsIgnoreCase(value, "x2")) g_Config.iMultiSampleLevel = 1;
        else if (EqualsIgnoreCase(value, "x4")) g_Config.iMultiSampleLevel = 2;
        else if (EqualsIgnoreCase(value, "x8")) g_Config.iMultiSampleLevel = 3;
        if (previous != g_Config.iMultiSampleLevel && PSP_IsInited() && gpu != nullptr) {
            const DisplayLayoutConfig &layout = g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation());
            gpu->NotifyRenderResized(layout);
        }
        return;
    }
    if (key == "ppsspp_cropto16x9") {
        const bool previous = g_Config.bDisplayCropTo16x9;
        if (hasBool) g_Config.bDisplayCropTo16x9 = on;
        if (previous != g_Config.bDisplayCropTo16x9) NotifyGpuDisplayResized();
        return;
    }
    if (key == "ppsspp_frameskip") {
        if (EqualsIgnoreCase(value, "disabled")) g_Config.iFrameSkip = 0;
        else if (ParseIntInRange(value, 0, 9, &number)) g_Config.iFrameSkip = number;
        return;
    }
    if (key == "ppsspp_auto_frameskip") {
        if (hasBool) g_Config.bAutoFrameSkip = on;
        return;
    }
    if (key == "ppsspp_frame_duplication") {
        if (hasBool) g_Config.bRenderDuplicateFrames = on;
        return;
    }
    if (key == "ppsspp_detect_vsync_swap_interval") {
        // Purely a frontend notification in the libretro port; the native
        // frontend detects refresh changes itself, so accept and ignore.
        return;
    }
    if (key == "ppsspp_inflight_frames") {
        if (EqualsIgnoreCase(value, "No buffer")) g_Config.iInflightFrames = 1;
        else if (EqualsIgnoreCase(value, "Up to 1")) g_Config.iInflightFrames = 2;
        return;
    }
    if (key == "ppsspp_gpu_hardware_transform") {
        if (hasBool) g_Config.bHardwareTransform = on;
        return;
    }
    if (key == "ppsspp_texture_scaling_type") {
        const int previous = g_Config.iTexScalingType;
        if (EqualsIgnoreCase(value, "xbrz")) g_Config.iTexScalingType = 0;
        else if (EqualsIgnoreCase(value, "hybrid")) g_Config.iTexScalingType = 1;
        else if (EqualsIgnoreCase(value, "bicubic")) g_Config.iTexScalingType = 2;
        else if (EqualsIgnoreCase(value, "hybrid_bicubic")) g_Config.iTexScalingType = 3;
        if (previous != g_Config.iTexScalingType) NotifyGpuConfigChanged();
        return;
    }
    if (key == "ppsspp_texture_scaling_level") {
        const int previous = g_Config.iTexScalingLevel;
        if (EqualsIgnoreCase(value, "disabled")) g_Config.iTexScalingLevel = 1;
        else if (EqualsIgnoreCase(value, "2x")) g_Config.iTexScalingLevel = 2;
        else if (EqualsIgnoreCase(value, "3x")) g_Config.iTexScalingLevel = 3;
        else if (EqualsIgnoreCase(value, "4x")) g_Config.iTexScalingLevel = 4;
        else if (EqualsIgnoreCase(value, "5x")) g_Config.iTexScalingLevel = 5;
        else if (ParseIntInRange(value, 1, 5, &number)) g_Config.iTexScalingLevel = number;
        if (previous != g_Config.iTexScalingLevel) NotifyGpuConfigChanged();
        return;
    }
    if (key == "ppsspp_texture_deposterize") {
        if (hasBool) g_Config.bTexDeposterize = on;
        return;
    }
    if (key == "ppsspp_texture_shader") {
        const std::string previous = g_Config.sTextureShaderName;
        if (EqualsIgnoreCase(value, "disabled")) g_Config.sTextureShaderName = "Off";
        else if (EqualsIgnoreCase(value, "2xBRZ")) g_Config.sTextureShaderName = "Tex2xBRZ";
        else if (EqualsIgnoreCase(value, "4xBRZ")) g_Config.sTextureShaderName = "Tex4xBRZ";
        else if (EqualsIgnoreCase(value, "MMPX")) g_Config.sTextureShaderName = "TexMMPX";
        g_Config.bTexHardwareScaling = g_Config.sTextureShaderName != "Off";
        if (previous != g_Config.sTextureShaderName) NotifyGpuConfigChanged();
        return;
    }
    if (key == "ppsspp_texture_anisotropic_filtering") {
        if (EqualsIgnoreCase(value, "disabled")) g_Config.iAnisotropyLevel = 0;
        else if (EqualsIgnoreCase(value, "2x")) g_Config.iAnisotropyLevel = 1;
        else if (EqualsIgnoreCase(value, "4x")) g_Config.iAnisotropyLevel = 2;
        else if (EqualsIgnoreCase(value, "8x")) g_Config.iAnisotropyLevel = 3;
        else if (EqualsIgnoreCase(value, "16x")) g_Config.iAnisotropyLevel = 4;
        return;
    }
    if (key == "ppsspp_texture_filtering") {
        if (EqualsIgnoreCase(value, "Auto")) g_Config.iTexFiltering = TEX_FILTER_AUTO;
        else if (EqualsIgnoreCase(value, "Nearest")) g_Config.iTexFiltering = TEX_FILTER_FORCE_NEAREST;
        else if (EqualsIgnoreCase(value, "Linear")) g_Config.iTexFiltering = TEX_FILTER_FORCE_LINEAR;
        else if (EqualsIgnoreCase(value, "Auto max quality")) g_Config.iTexFiltering = TEX_FILTER_AUTO_MAX_QUALITY;
        return;
    }
    if (key == "ppsspp_smart_2d_texture_filtering") {
        if (hasBool) g_Config.bSmart2DTexFiltering = on;
        return;
    }
    if (key == "ppsspp_texture_replacement") {
        if (hasBool) g_Config.bReplaceTextures = on;
        return;
    }

    // ----- Input -----
    if (key == "ppsspp_button_preference") {
        if (EqualsIgnoreCase(value, "Cross")) g_Config.iButtonPreference = PSP_SYSTEMPARAM_BUTTON_CROSS;
        else if (EqualsIgnoreCase(value, "Circle")) g_Config.iButtonPreference = PSP_SYSTEMPARAM_BUTTON_CIRCLE;
        return;
    }
    if (key == "ppsspp_analog_is_circular") {
        if (hasBool) g_Config.bAnalogIsCircular = on;
        return;
    }
    if (key == "ppsspp_analog_deadzone") {
        char *end = nullptr;
        const float parsed = strtof(value.c_str(), &end);
        if (end != value.c_str() && *end == '\0') g_Config.fAnalogDeadzone = std::clamp(parsed, 0.0f, 1.0f);
        return;
    }
    if (key == "ppsspp_analog_sensitivity") {
        char *end = nullptr;
        const float parsed = strtof(value.c_str(), &end);
        if (end != value.c_str() && *end == '\0') g_Config.fAnalogSensitivity = std::clamp(parsed, 0.1f, 10.0f);
        return;
    }

    // ----- Hacks -----
    if (key == "ppsspp_skip_buffer_effects") {
        if (hasBool) g_Config.bSkipBufferEffects = on;
        return;
    }
    if (key == "ppsspp_skip_gpu_readbacks") {
        if (hasBool) g_Config.iSkipGPUReadbackMode = on ? 1 : 0;
        return;
    }
    if (key == "ppsspp_spline_quality") {
        if (EqualsIgnoreCase(value, "Low")) g_Config.iSplineBezierQuality = 0;
        else if (EqualsIgnoreCase(value, "Medium")) g_Config.iSplineBezierQuality = 1;
        else if (EqualsIgnoreCase(value, "High")) g_Config.iSplineBezierQuality = 2;
        return;
    }
    if (key == "ppsspp_lower_resolution_for_effects") {
        if (EqualsIgnoreCase(value, "disabled")) g_Config.iBloomHack = 0;
        else if (EqualsIgnoreCase(value, "Safe")) g_Config.iBloomHack = 1;
        else if (EqualsIgnoreCase(value, "Balanced")) g_Config.iBloomHack = 2;
        else if (EqualsIgnoreCase(value, "Aggressive")) g_Config.iBloomHack = 3;
        return;
    }

    // ----- Network -----
    if (key == "ppsspp_enable_wlan") {
        if (hasBool) g_Config.bEnableWlan = on;
        return;
    }
    if (key == "ppsspp_wlan_channel") {
        if (EqualsIgnoreCase(value, "Auto")) g_Config.iWlanAdhocChannel = PSP_SYSTEMPARAM_ADHOC_CHANNEL_AUTOMATIC;
        else if (ParseIntInRange(value, 1, 11, &number)) g_Config.iWlanAdhocChannel = number;
        return;
    }
    if (key == "ppsspp_enable_builtin_pro_ad_hoc_server") {
        if (hasBool) g_Config.bEnableAdhocServer = on;
        return;
    }
    if (key == "ppsspp_change_pro_ad_hoc_server_address") {
        g_adhocUseIpAddress = EqualsIgnoreCase(value, "IP address");
        if (!g_adhocUseIpAddress) g_Config.sProAdhocServer = value;
        else {
            EnsureAdhocDigits();
            RebuildAdhocIp();
        }
        return;
    }
    if (key == "ppsspp_enable_upnp") {
        if (hasBool) g_Config.bEnableUPnP = on;
        return;
    }
    if (key == "ppsspp_upnp_use_original_port") {
        if (hasBool) g_Config.bUPnPUseOriginalPort = on;
        return;
    }
    if (key == "ppsspp_port_offset") {
        if (ParseIntInRange(value, 0, 65535, &number)) g_Config.iPortOffset = number;
        return;
    }
    if (key == "ppsspp_minimum_timeout") {
        if (ParseIntInRange(value, 0, 60000, &number)) g_Config.iMinTimeout = number;
        return;
    }
    if (key == "ppsspp_forced_first_connect") {
        if (hasBool) g_Config.bForcedFirstConnect = on;
        return;
    }
    if (key.rfind("ppsspp_change_mac_address", 0) == 0 && key.size() == 27) {
        const int index = atoi(key.substr(25).c_str()) - 1;
        if (index >= 0 && index < 12 && value.size() == 1 && std::isxdigit((unsigned char)value[0])) {
            EnsureMacDigits();
            g_macDigits[index] = static_cast<char>(std::tolower((unsigned char)value[0]));
            RebuildMacAddress();
        }
        return;
    }
    if (key.rfind("ppsspp_pro_ad_hoc_server_address", 0) == 0 && key.size() == 34) {
        const int index = atoi(key.substr(32).c_str()) - 1;
        if (index >= 0 && index < 12 && value.size() == 1 && std::isdigit((unsigned char)value[0])) {
            EnsureAdhocDigits();
            g_adhocDigits[index] = value[0] - '0';
            if (g_adhocUseIpAddress) RebuildAdhocIp();
        }
        return;
    }

    // Managed by the frontend's own renderer controls (nativeSetRenderer).
    if (key == "ppsspp_backend" || key == "ppsspp_software_rendering") return;

    NLOGW("Unknown config key: %s", key.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// JNI surface for com.sbro.emucorea.core.NativePpsspp.
// ---------------------------------------------------------------------------
extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    g_vm = vm;
    // Core-spawned loader/IO threads use AndroidJNIThreadContext; without this
    // registration the attach/detach hooks are no-ops and some devices kill
    // the process for JNI calls from unattached threads.
    RegisterAttachDetach(&EmuCoreA_AttachThreadToJNI, &EmuCoreA_DetachThreadFromJNI);
    EmuCoreAAchievementsSetJavaVm(vm);
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) {
        EmuCoreAAchievementsInitializeJava(env);
    }
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

#ifndef NDEBUG
    // PPSSPP's own log manager is muted until a frontend enables an output.
    g_Config.bEnableLogging = true;
    g_logManager.Init(&g_Config.bEnableLogging, false);
    g_logManager.SetOutputsEnabled(LogOutput::Stdio);
#endif

    // Must happen before anything reads g_Config (including CreateGraphicsContext
    // and the loader): otherwise every scalar keeps its zero value and the core
    // boots the interpreter with vsync off.
    InitializeConfigIfNeeded();

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
    if (game.empty()) {
        // PPSSPP's loader requires content to identify (an empty path fails
        // Identify_File before the kernel starts), so a disc-less boot cannot
        // succeed. Fail cleanly instead of starting a loader that dies.
        NLOGW("BIOS-only boot requested, but the core requires a game image");
        return JNI_FALSE;
    }
    // The core initializes the GPU on the loader thread, so surface and (for
    // OpenGL) draw context must be ready. The actual PSP_InitStart is queued
    // for the frame thread, which owns the GL context.
    std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
    if (!g_renderReady) {
        NLOGE("nativeBoot called before the surface was attached");
        return JNI_FALSE;
    }
    if (g_booted || g_pendingBoot || g_bootRequested) {
        NLOGW("nativeBoot called while a session is already booted or booting");
        return JNI_FALSE;
    }
    g_bootRequestPath = game;
    g_bootRequestLoader = nullptr;
    g_bootRequested = true;
    StartAudio();
    return JNI_TRUE;
}

// Boots a game that the frontend opened through Android's Storage Access
// Framework. The caller keeps ownership of its fd: FdFileLoader duplicates it,
// and the duplicate is closed by the core when the session shuts down. The
// pathHint only names the container (its extension drives Identify_File) and
// must not be used to reopen the file.
JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeBootFd(JNIEnv *env, jclass, jint fd,
                                                      jstring pathHint) {
    if (fd < 0) {
        NLOGW("nativeBootFd called with an invalid descriptor");
        return JNI_FALSE;
    }
    const std::string hint = ToString(env, pathHint);
    auto *loader = new FdFileLoader(fd, hint);
    if (!loader->Exists()) {
        NLOGE("nativeBootFd: descriptor is not a readable regular file");
        delete loader;
        return JNI_FALSE;
    }
    std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
    if (!g_renderReady) {
        NLOGE("nativeBootFd called before the surface was attached");
        delete loader;
        return JNI_FALSE;
    }
    if (g_booted || g_pendingBoot || g_bootRequested) {
        NLOGW("nativeBootFd called while a session is already booted or booting");
        delete loader;
        return JNI_FALSE;
    }
    g_bootRequestPath = hint;
    g_bootRequestLoader = loader;
    g_bootRequested = true;
    StartAudio();
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetSurface(JNIEnv *env, jclass, jobject surface,
                                                          jint width, jint height) {
    if (surface == nullptr) {
        std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
        if (g_renderReady) {
            if (gpu != nullptr) gpu->DeviceLost();
            if (g_activeRenderer == kRendererOpenGL) {
                ShutdownGLOnFrameThread();
                DestroyEGLSurface();
            } else {
                g_graphicsContext->ShutdownSurface();
            }
            g_renderReady = false;
        }
        if (g_glWindow != nullptr) {
            ANativeWindow_release(g_glWindow);
            g_glWindow = nullptr;
        }
        if (g_surfaceWindow != nullptr) {
            ANativeWindow_release(g_surfaceWindow);
            g_surfaceWindow = nullptr;
        }
        g_surfaceWidth = 0;
        g_surfaceHeight = 0;
        return JNI_TRUE;
    }
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
    ANativeWindow_acquire(window);
    if (g_surfaceWindow != nullptr) ANativeWindow_release(g_surfaceWindow);
    g_surfaceWindow = window;
    g_surfaceWidth = width;
    g_surfaceHeight = height;
    const bool ok = AttachSurface(window, width, height);
    ANativeWindow_release(window);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeRunFrame(JNIEnv *, jclass) {
    RunFrame();
    if (g_corePoweredDown) {
        g_corePoweredDown = false;
        // Sentinel: the game exited itself; -1 is never a valid emulated time.
        return -1;
    }
    return g_booted ? static_cast<jlong>(CoreTiming::GetGlobalTimeUs()) : 0;
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetPadButtons(JNIEnv *, jclass, jint port, jint buttons) {
    // The PSP exposes a single controller; port 1 is accepted for API
    // compatibility but the core only has CTRL port 0.
    if (port != 0) return;
    // The frontend sends the complete button state in the same active-low
    // bitmap PPSSPP uses for its PSP pads (bit set = released); the standard
    // buttons all live in the low 16 bits. HOME/HOLD/WLAN and the other high
    // bits are not frontend-controlled and must stay untouched.
    constexpr u32 kFrontendPadMask = 0xFFFF;
    const u32 released = static_cast<u32>(buttons) & kFrontendPadMask;
    __CtrlUpdateButtons((~released) & kFrontendPadMask, released);
}

// The frontend packs each axis as an unsigned byte with 0x80 as the center
// (matching PPSSPP's Android pad axis convention). __CtrlSetAnalogXY expects
// -1..1 with +Y up, so Y is negated here.
static float PadAxisToFloat(int value) {
    return (static_cast<float>(value) - 127.5f) / 127.5f;
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetPadAnalog(JNIEnv *, jclass, jint port, jint lx,
                                                            jint ly, jint rx, jint ry) {
    if (port != 0) return;
    __CtrlSetAnalogXY(CTRL_STICK_LEFT, PadAxisToFloat(lx), -PadAxisToFloat(ly));
    __CtrlSetAnalogXY(CTRL_STICK_RIGHT, PadAxisToFloat(rx), -PadAxisToFloat(ry));
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeShutdown(JNIEnv *, jclass) {
    ShutdownCore();
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetConfig(JNIEnv *env, jclass, jstring key,
                                                         jstring value) {
    const std::string k = ToString(env, key);
    const std::string v = ToString(env, value);
    if (k.empty()) return;
    std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
    ApplyNativeConfig(k, v);
}

// Selects the renderer for the next graphics context. Values follow
// RendererDefaults' core contract: 0 = software, 1 = Vulkan, 2 = OpenGL ES.
// The core is only recreated while idle; during a session the choice is stored
// and applied when the next session creates its context.
JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetRenderer(JNIEnv *, jclass, jint renderer) {
    const int normalized = renderer == kRendererOpenGL || renderer == kRendererSoftware
                               ? renderer
                               : kRendererVulkan;
    std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
    g_requestedRenderer.store(normalized);
    NLOGI("Renderer requested: %s (%d)", RendererName(normalized), normalized);

    if (g_graphicsContext == nullptr) {
        return;
    }
    if (g_booted || g_pendingBoot || g_bootRequested) {
        NLOGI("Renderer change stored; it applies when the next session starts");
        return;
    }
    // Idle: rebuild the context now so the choice is honored before PSP_Init.
    // A surface may already be attached (launch options arrive after the
    // SurfaceView is created), so remember that and re-attach afterwards.
    const bool hadSurface = g_renderReady;
    if (g_graphicsContext != nullptr) {
        if (g_renderReady) {
            if (g_activeRenderer == kRendererOpenGL) {
                ShutdownGLOnFrameThread();
                DestroyEGLSurface();
            } else {
                g_graphicsContext->ShutdownSurface();
            }
            g_renderReady = false;
        }
        g_graphicsContext->ShutdownAPI();
        delete g_graphicsContext;
        g_graphicsContext = nullptr;
        Core_SetGraphicsContext(nullptr);
    }
    CreateGraphicsContext();
    if (hadSurface && g_surfaceWindow != nullptr) {
        AttachSurface(g_surfaceWindow, g_surfaceWidth, g_surfaceHeight);
    }
}

// Reports the renderer the core actually initialized (0 = software,
// 1 = Vulkan, 2 = OpenGL ES). Used by the frontend overlay, which must not
// claim a backend that failed to come up.
JNIEXPORT jint JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeGetActiveRenderer(JNIEnv *, jclass) {
    std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
    return g_activeRenderer;
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetCheats(JNIEnv *env, jclass, jstring path) {
    const std::string cheatPath = ToString(env, path);
    std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
    if (cheatPath.empty()) {
        g_pendingCheatFile.clear();
        g_Config.bEnableCheats = false;
        g_Config.bReloadCheats = false;
        NLOGI("Cheats disabled");
        return;
    }
    g_pendingCheatFile = cheatPath;
    ApplyPendingCheatsLocked();
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetShaderEffect(JNIEnv *, jclass, jint effect) {
    g_shaderEffect = effect;
    // The actual chain is driven by the preset; the effect id only classifies
    // the selected pack for logging and the built-in fallback.
    emucorer::shader_chain::SetPreset(g_shaderPreset, !g_shaderPreset.empty());
    NLOGI("Shader effect stored: %d (chain %s)", g_shaderEffect,
          emucorer::shader_chain::IsEnabled() ? "enabled" : "disabled");
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetShaderPreset(JNIEnv *env, jclass, jstring preset) {
    g_shaderPreset = ToString(env, preset);
    // Publishing a new generation makes the presentation rebuild its
    // librashader chain on the render thread; an empty path disables it and
    // turns the next present into a plain pass-through.
    emucorer::shader_chain::SetPreset(g_shaderPreset, !g_shaderPreset.empty());
    NLOGI("Shader preset stored: %s (chain %s)", g_shaderPreset.c_str(),
          emucorer::shader_chain::IsEnabled() ? "enabled" : "disabled");
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetRewindEnabled(JNIEnv *, jclass, jboolean enabled) {
    const bool on = enabled == JNI_TRUE;
    g_Config.iRewindSnapshotInterval = on ? 2 : 0;
    if (!on) g_clearRewindRequested = true;
    NLOGI("Rewind %s", on ? "enabled" : "disabled");
}

JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeRewindStep(JNIEnv *, jclass) {
    if (!PSP_IsInited() || g_Config.iRewindSnapshotInterval <= 0 || !SaveState::CanRewind()) {
        return JNI_FALSE;
    }
    SaveState::Rewind();
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSaveState(JNIEnv *env, jclass, jstring path) {
    if (!g_booted) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
    return SaveStateToFile(ToString(env, path)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeLoadState(JNIEnv *env, jclass, jstring path) {
    if (!g_booted) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
    return LoadStateFromFile(ToString(env, path)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jintArray JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeGetFrameSize(JNIEnv *env, jclass) {
    // The overlay shows the game's internal rendering resolution, not the
    // window/backbuffer size.
    jint values[2] = {static_cast<jint>(PSP_CoreParameter().renderWidth),
                      static_cast<jint>(PSP_CoreParameter().renderHeight)};
    jintArray result = env->NewIntArray(2);
    if (result != nullptr) env->SetIntArrayRegion(result, 0, 2, values);
    return result;
}

JNIEXPORT jlong JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeGetMemoryPointer(JNIEnv *, jclass) {
    return reinterpret_cast<jlong>(Memory::GetPointerWriteUnchecked(PSP_GetKernelMemoryBase()));
}

JNIEXPORT jlong JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeGetMemorySize(JNIEnv *, jclass) {
    return static_cast<jlong>(Memory::g_MemorySize);
}

}  // extern "C"

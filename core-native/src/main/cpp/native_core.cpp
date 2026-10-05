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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
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
// Cheat .ini requested by the frontend, applied once the disc ID is known.
std::string g_pendingCheatFile;
// Rewind history must be dropped on the frame thread (ring buffer + compressor).
bool g_clearRewindRequested = false;
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

extern "C" bool EmuCoreANativeAchievementHash(const char *path, char *hash) {
    if (path == nullptr || *path == '\0' || hash == nullptr) return false;

    FileLoader *loader = ConstructFileLoader(Path(path));
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
        // PPSSPP's OpenGL backend expects an EGL context that is current on the
        // render thread (its own Android app creates it from Java). This
        // frontend has no EGL render-thread integration, so GLES cannot be
        // brought up safely here; keep the app working on Vulkan.
        NLOGE("OpenGL ES is not supported by the native frontend; using Vulkan instead");
    } else if (requested == kRendererSoftware) {
        NLOGW("Software rendering is not supported by the native frontend; using Vulkan instead");
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
    std::string error;
    if (!g_graphicsContext->InitSurface(WINDOWSYSTEM_ANDROID, window, nullptr, &error)) {
        NLOGE("InitSurface failed: %s", error.c_str());
        return false;
    }
    // InitSurface() already created the swapchain, the draw context, its
    // presets and the backbuffer event. Calling any of those again starts the
    // render threads twice, which terminates the process.
    if (g_graphicsContext->GetDrawContext() == nullptr) return false;

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

#if defined(EMUCOREA_HAVE_LIBRASHADER)
    // The shader-chain presentation is strictly opt-in: with no preset enabled
    // the core must use PPSSPP's untouched real-swapchain path. Installing it
    // unconditionally changed behavior even in pass-through mode.
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
#endif

    g_renderReady = true;
    NLOGI("Surface attached %dx%d", width, height);
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

    ApplyPendingCheatsLocked();

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
    if (g_clearRewindRequested) {
        g_clearRewindRequested = false;
        SaveState::ClearRewind();
    }
    SaveState::Process();
    EmuCoreAAchievementsOnFrame();
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
    if (g_booted) {
        PSP_Shutdown(true);
        g_booted = false;
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
    // The non-libretro loader thread initializes the GPU immediately, so the
    // draw context (created with the surface) must already exist.
    if (!g_renderReady) {
        NLOGE("nativeBoot called before the surface was attached");
        return JNI_FALSE;
    }
    StartAudio();
    return BootGame(game, nullptr) ? JNI_TRUE : JNI_FALSE;
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
    if (!g_renderReady) {
        NLOGE("nativeBootFd called before the surface was attached");
        return JNI_FALSE;
    }
    const std::string hint = ToString(env, pathHint);
    auto *loader = new FdFileLoader(fd, hint);
    if (!loader->Exists()) {
        NLOGE("nativeBootFd: descriptor is not a readable regular file");
        delete loader;
        return JNI_FALSE;
    }
    StartAudio();
    return BootGame(hint, loader) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetSurface(JNIEnv *env, jclass, jobject surface,
                                                          jint width, jint height) {
    if (surface == nullptr) {
        std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
        if (g_renderReady) {
            if (gpu != nullptr) gpu->DeviceLost();
            g_graphicsContext->ShutdownSurface();
            g_renderReady = false;
        }
        return JNI_TRUE;
    }
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(g_nativeFrameMutex);
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

JNIEXPORT void JNICALL
Java_com_sbro_emucorea_core_NativePpsspp_nativeSetConfig(JNIEnv *env, jclass, jstring key,
                                                         jstring value) {
    const std::string k = ToString(env, key);
    const std::string v = ToString(env, value);
    const bool on = v == "1" || v == "true";
    if (k == "internal_resolution") {
        g_Config.iInternalResolution = std::clamp(atoi(v.c_str()), 1, 10);
    } else if (k == "frameskip") {
        g_Config.iFrameSkip = atoi(v.c_str());
    } else if (k == "auto_frameskip") {
        g_Config.bAutoFrameSkip = on;
    } else if (k == "frame_duplication") {
        g_Config.bRenderDuplicateFrames = on;
    } else if (k == "texture_filtering") {
        g_Config.iTexFiltering = atoi(v.c_str());
    } else if (k == "texture_scaling_level") {
        g_Config.iTexScalingLevel = atoi(v.c_str());
    } else if (k == "volume") {
        g_Config.iGameVolume = std::clamp(atoi(v.c_str()), 0, 100);
    } else if (k == "skip_buffer_effects") {
        g_Config.bSkipBufferEffects = on;
    } else if (k == "fast_memory") {
        g_Config.bFastMemory = on;
    } else if (k == "cpu_core") {
        g_Config.iCpuCore = v == "jit" ? (int)CPUCore::JIT : (int)CPUCore::IR_INTERPRETER;
    } else if (k == "crop16x9") {
        g_Config.bDisplayCropTo16x9 = on;
    } else if (k == "vsync") {
        g_Config.bVSync = on;
    } else if (k == "multi_threading") {
        g_Config.bRenderMultiThreading = on;
    } else {
        NLOGW("Unknown config key: %s", k.c_str());
    }
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
    if (g_booted || g_pendingBoot || g_renderReady) {
        NLOGI("Renderer change stored; it applies when the next session starts");
        return;
    }
    // Idle: rebuild the context now so the choice is honored before PSP_Init.
    g_graphicsContext->ShutdownAPI();
    delete g_graphicsContext;
    g_graphicsContext = nullptr;
    Core_SetGraphicsContext(nullptr);
    CreateGraphicsContext();
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
    jint values[2] = {static_cast<jint>(PSP_CoreParameter().pixelWidth),
                      static_cast<jint>(PSP_CoreParameter().pixelHeight)};
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

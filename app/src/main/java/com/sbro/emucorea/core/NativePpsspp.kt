// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreA-Proprietary
package com.sbro.emucorea.core

import android.content.Context
import android.util.Log
import android.view.Surface

/**
 * JNI surface of the EmuCoreA native PPSSPP core (libemucorea_core.so).
 *
 * This is the non-libretro frontend: the core is driven directly with the same
 * graphics/audio backends the standalone PPSSPP app uses. EmuCoreA owns the UI
 * and the frame loop; no PPSSPP ImGui UI is involved.
 */
object NativePpsspp {
    private const val TAG = "EmuCoreA-Native"

    @Volatile
    private var loaded = false

    @Synchronized
    fun ensureLoaded(): Boolean {
        if (loaded) return true
        return try {
            System.loadLibrary("emucorea_core")
            loaded = true
            Log.i(TAG, "libemucorea_core loaded")
            true
        } catch (error: Throwable) {
            Log.e(TAG, "Failed to load libemucorea_core", error)
            false
        }
    }

    /**
     * Hands the application context to the core. PPGe dialogs (savedata,
     * memory stick, OSK) draw their text through the Java TextRenderer, which
     * loads the fonts from the APK assets and needs a Context.
     */
    external fun nativeSetAppContext(context: Context)

    external fun nativeSetCustomDriver(path: String, nativeLibDir: String)
    external fun nativeIsCustomDriverLoaded(): Boolean

    /**
     * Swap the custom Vulkan driver while a game runs. path = "" -> system driver.
     * Returns RELOAD_APPLIED=0, RELOAD_FELL_BACK_TO_SYSTEM=1, RELOAD_FAILED=2, RELOAD_STORED=3
     * (STORED = no running Vulkan session; the path is just stored for the next context/launch).
     * Blocking, call off the main thread.
     */
    @JvmStatic external fun nativeReloadGpuDriver(path: String, nativeLibDir: String): Int

    /**
     * Short label of the driver the active Vulkan device really uses, e.g. "Turnip Mesa 25.3" /
     * "Mali-SX2 ..." / "Qualcomm 512.744"; max 28 chars; "" when the renderer is not Vulkan or
     * nothing is initialised.
     */
    @JvmStatic external fun nativeGetDriverLabel(): String

    external fun nativeInit(
        apkPath: String,
        dataDir: String,
        externalDir: String,
        cacheDir: String,
        displayWidth: Int,
        displayHeight: Int,
        refreshRate: Float
    )

    external fun nativeBoot(gamePath: String): Boolean

    /**
     * Boots a game opened through Android's Storage Access Framework.
     *
     * The core duplicates [fd] and reads through pread64; the caller keeps
     * ownership of [fd] and must keep it open for the whole session. The core
     * closes its duplicate when the session shuts down. [pathHint] names the
     * container (for example the SAF display name) so the core can detect the
     * image format from the extension; it is never used to reopen the file.
     */
    external fun nativeBootFd(fd: Int, pathHint: String): Boolean

    /**
     * Reads a game asset through the core's loader stack, which understands
     * every container the emulator can boot (ISO, CSO, CHD and ZIP-wrapped
     * images). [asset] 0 = PARAM.SFO, 1 = ICON0.PNG. [fd] >= 0 reads through
     * a SAF descriptor whose ownership stays with the caller; otherwise the
     * file at [path] is opened directly.
     */
    external fun nativeReadGameAsset(path: String, fd: Int, asset: Int): ByteArray?

    /**
     * Reads the PSP firmware version (for example "6.60") of the updater in the
     * file behind [fd], or "" if the file carries none. Accepted inputs: a game
     * disc image (ISO/CSO/CHD) with PSP_GAME/SYSDIR/UPDATE/DATA.BIN, an updater
     * EBOOT.PBP, or a bare DATA.BIN / PSAR file. Cheap: reads only the updater's
     * PARAM.SFO and the first 64 KiB of the archive, never decrypts the payload.
     *
     * Blocking; call off the main thread. The caller keeps ownership of [fd]
     * (the core works on a duplicate). [pathHint] is the display name, used only
     * for its extension. Needs neither nativeInit nor a running game, and does not
     * touch emulator state.
     */
    @JvmStatic
    external fun nativeReadUpdaterVersionFd(fd: Int, pathHint: String): String

    /**
     * Unpacks the firmware inside the updater behind [fd] (same input shapes as
     * [nativeReadUpdaterVersionFd]) into [outputDir], a plain filesystem path in
     * app-private storage. Only entries whose firmware path starts with
     * [prefixFilter] (case-insensitive, e.g. "flash0:/"; empty = everything) are
     * written, for every PSP model's file list.
     *
     * Layout: a "flash0:/x/y" entry is written to `<outputDir>/flash0/x/y`, so with
     * "flash0:/" the caller must treat `<outputDir>/flash0` as flash0 (fonts in
     * `<outputDir>/flash0/font`, XMB resources in
     * `<outputDir>/flash0/vsh/resource`). [outputDir] itself is not flash0.
     * Entries no file list names are skipped by a non-empty filter.
     *
     * [progress] receives 0..100 on the calling thread (may be null; values are
     * monotonic, 100 only on success). Returns "" on success, otherwise a short
     * English error. Blocking and CPU/IO heavy (the whole archive, up to ~100 MB,
     * is held in memory); call off the main thread. The caller keeps ownership of
     * [fd]. Needs neither nativeInit nor a running game; it touches no emulator
     * state, so it is also safe during a session, just slow and memory hungry.
     * Not cancellable. Use a fresh or empty [outputDir]; existing files are overwritten.
     */
    @JvmStatic
    external fun nativeUnpackUpdaterFd(
        fd: Int,
        pathHint: String,
        outputDir: String,
        prefixFilter: String,
        progress: java.util.function.IntConsumer?,
    ): String

    external fun nativeSetSurface(surface: Surface?, width: Int, height: Int): Boolean

    external fun nativeRunFrame(): Long

    external fun nativeSetPadButtons(port: Int, buttons: Int)

    external fun nativeSetPadAnalog(port: Int, lx: Int, ly: Int, rx: Int, ry: Int)

    external fun nativeSetConfig(key: String, value: String)

    /**
     * Android audio device properties from [android.media.AudioManager]
     * (PROPERTY_OUTPUT_SAMPLE_RATE / PROPERTY_OUTPUT_FRAMES_PER_BUFFER).
     * The core sizes the output stream and its resampler ring from these,
     * exactly like PPSSPP's own Android audio init. Zero means "unknown".
     */
    external fun nativeSetAudioDeviceInfo(sampleRate: Int, framesPerBuffer: Int)

    /**
     * Selects the renderer used when the graphics context is created, using
     * [RendererDefaults]' core values (0 = software, 1 = Vulkan, 2 = OpenGL ES).
     * Call before [nativeInit] or before a session starts; during a session the
     * choice is applied to the next one.
     */
    external fun nativeSetRenderer(renderer: Int)

    /**
     * Renderer the core actually initialized: 0 = software, 1 = Vulkan,
     * 2 = OpenGL ES. The overlay and diagnostics must report this instead of
     * the stored preference.
     */
    external fun nativeGetActiveRenderer(): Int

    /** True once the asynchronous core boot (PSP_InitUpdate) has completed. */
    external fun nativeIsBooted(): Boolean

    /** Empty while healthy; the native boot/render failure otherwise. */
    external fun nativeGetRuntimeError(): String

    /** Suspends device audio along with the frontend frame loop. */
    external fun nativeSetPaused(paused: Boolean)

    /** Repoints the core at a new data root while idle. */
    external fun nativeUpdateDataDirectories(dataDir: String, externalDir: String)

    external fun nativeSetCheats(path: String)

    external fun nativeSetRewindEnabled(enabled: Boolean)

    external fun nativeRewindStep(): Boolean

    /** Resumes the core after the rewind control is released. */
    external fun nativeRewindRelease()

    /**
     * PPSSPP frame statistics: [0] = emulated vblanks/s (speed source),
     * [1] = displayed flips/s, [2] = actual displayed fps.
     */
    external fun nativeGetDisplayStats(): FloatArray?

    external fun nativeSetShaderPreset(preset: String)

    external fun nativeSaveState(path: String): Boolean

    external fun nativeLoadState(path: String): Boolean

    /** Save and return the exact core error (empty on success). Debug/tests. */
    external fun nativeSaveStateDebug(path: String): String

    /** Load and return the exact core error (empty on success). Debug/tests. */
    external fun nativeLoadStateDebug(path: String): String

    external fun nativeGetFrameSize(): IntArray?

    external fun nativeAchievementsSetEnabled(enabled: Boolean)

    external fun nativeAchievementsSetHardcore(enabled: Boolean)

    external fun nativeAchievementsSetUnofficial(enabled: Boolean)

    external fun nativeAchievementsSetEncore(enabled: Boolean)

    external fun nativeAchievementsLoginWithPassword(user: String, password: String): String?

    external fun nativeAchievementsLoginWithToken(user: String, token: String): String?

    external fun nativeAchievementsLogout()

    external fun nativeAchievementsLoadGame(path: String)

    external fun nativeAchievementsUnloadGame()

    external fun nativeAchievementsStateJson(): String

    external fun nativeAchievementsAchievementsJson(): String

    external fun nativeAchievementsPollEventsJson(): String

    external fun nativeShutdown()
}

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

    external fun nativeSetShaderEffect(effect: Int)

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

// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreA-Proprietary
package com.sbro.emucorea.core

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

    external fun nativeSetCheats(path: String)

    external fun nativeSetRewindEnabled(enabled: Boolean)

    external fun nativeRewindStep(): Boolean

    external fun nativeSetShaderEffect(effect: Int)

    external fun nativeSetShaderPreset(preset: String)

    external fun nativeSaveState(path: String): Boolean

    external fun nativeLoadState(path: String): Boolean

    external fun nativeGetFrameSize(): IntArray?

    external fun nativeGetMemoryPointer(): Long

    external fun nativeGetMemorySize(): Long

    external fun nativeAchievementsSetEnabled(enabled: Boolean)

    external fun nativeAchievementsSetHardcore(enabled: Boolean)

    external fun nativeAchievementsSetUnofficial(enabled: Boolean)

    external fun nativeAchievementsSetEncore(enabled: Boolean)

    external fun nativeAchievementsLoginWithPassword(user: String, password: String): String?

    external fun nativeAchievementsLoginWithToken(user: String, token: String): String?

    external fun nativeAchievementsLogout()

    external fun nativeAchievementsLoadGame(path: String)

    external fun nativeAchievementsUnloadGame()

    external fun nativeAchievementsPump()

    external fun nativeAchievementsStateJson(): String

    external fun nativeAchievementsAchievementsJson(): String

    external fun nativeAchievementsPollEventsJson(): String

    external fun nativeShutdown()
}

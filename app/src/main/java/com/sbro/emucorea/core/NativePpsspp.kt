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

    external fun nativeSetSurface(surface: Surface?, width: Int, height: Int): Boolean

    external fun nativeRunFrame(): Long

    external fun nativeSetPadButtons(port: Int, buttons: Int)

    external fun nativeSetPadAnalog(port: Int, lx: Int, ly: Int, rx: Int, ry: Int)

    external fun nativeSetConfig(key: String, value: String)

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

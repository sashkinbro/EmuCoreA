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

    external fun nativeShutdown()
}

// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreA-Proprietary
package com.sbro.emucorea

import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.MotionEvent
import android.app.Activity
import android.view.SurfaceHolder
import android.view.SurfaceView
import com.sbro.emucorea.core.NativePpsspp

/**
 * Debug probe for the EmuCoreA native PPSSPP core (no libretro, no PPSSPP UI).
 *
 * adb shell am start -n com.sbro.emucorea/com.sbro.emucorea.NativeCoreProbeActivity \
 *   --es game "/storage/emulated/0/PSP Games/Game.iso"
 */
class NativeCoreProbeActivity : Activity(), SurfaceHolder.Callback {
    @Volatile private var running = false
    @Volatile private var surfaceReady = false
    private var frameThread: Thread? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val gamePath = intent.getStringExtra("game").orEmpty()
        if (!NativePpsspp.ensureLoaded()) {
            finish()
            return
        }

        val metrics = resources.displayMetrics
        val refresh = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display?.refreshRate ?: 60.0f
        } else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay.refreshRate
        }

        NativePpsspp.nativeInit(
            packageCodePath,
            filesDir.absolutePath + "/ppsspp-native",
            getExternalFilesDir(null)?.absolutePath ?: filesDir.absolutePath,
            cacheDir.absolutePath,
            metrics.widthPixels,
            metrics.heightPixels,
            refresh
        )

        val surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        setContentView(surfaceView)

        running = true
        frameThread = Thread {
            while (running && !surfaceReady) {
                Thread.sleep(10)
            }
            if (!running) return@Thread
            if (!NativePpsspp.nativeBoot(gamePath)) {
                Log.e(TAG, "nativeBoot failed for $gamePath")
                running = false
                return@Thread
            }
            Log.i(TAG, "native boot started, entering frame loop")
            while (running) {
                NativePpsspp.nativeRunFrame()
            }
        }.also { it.name = "EmuCoreA-NativeFrame"; it.start() }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        surfaceReady = NativePpsspp.nativeSetSurface(
            holder.surface, holder.surfaceFrame.width(), holder.surfaceFrame.height()
        )
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        if (!surfaceReady) {
            surfaceReady = NativePpsspp.nativeSetSurface(holder.surface, width, height)
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        NativePpsspp.nativeSetSurface(null, 0, 0)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean = true

    override fun onDestroy() {
        running = false
        frameThread?.join(2000)
        frameThread = null
        NativePpsspp.nativeShutdown()
        super.onDestroy()
    }

    companion object {
        private const val TAG = "NativeCoreProbe"
    }
}

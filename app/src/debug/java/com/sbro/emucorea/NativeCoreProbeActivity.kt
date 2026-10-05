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

        // Optional renderer override for debugging: 0 = software, 1 = Vulkan,
        // 2 = OpenGL ES. Must be set before nativeInit creates the context.
        if (intent.hasExtra("renderer")) {
            NativePpsspp.nativeSetRenderer(intent.getIntExtra("renderer", 1))
        }

        // Optional shader preset override. Must be set before the surface is
        // attached so the shader-chain presentation is installed with it.
        intent.getStringExtra("shaderPreset")?.let(NativePpsspp::nativeSetShaderPreset)

        // Optional rewind self-test: accumulates snapshots, rewinds three
        // times, releases, and logs the emulated clock around every step.
        val rewindTest = intent.getBooleanExtra("rewind", false)
        if (rewindTest) {
            NativePpsspp.nativeSetRewindEnabled(true)
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
            var rewindPhasesLeft = if (rewindTest) 3 else 0
            var rewindInitiated = false
            var emuUs = 0L
            var emuAtRewindStart = 0L
            while (running) {
                emuUs = NativePpsspp.nativeRunFrame()
                if (rewindTest && !rewindInitiated && emuUs >= 6_000_000L) {
                    rewindInitiated = true
                    emuAtRewindStart = emuUs
                    Log.i(TAG, "REWIND test start at emu=${emuUs / 1000}ms")
                }
                if (rewindInitiated && rewindPhasesLeft > 0 && emuUs > 0L) {
                    rewindPhasesLeft--
                    val before = emuUs
                    val stepped = NativePpsspp.nativeRewindStep()
                    Thread.sleep(700)
                    Log.i(TAG, "REWIND step result=$stepped before=${before / 1000}ms")
                    if (rewindPhasesLeft == 0) {
                        NativePpsspp.nativeRewindRelease()
                        Thread.sleep(1500)
                        val after = NativePpsspp.nativeRunFrame()
                        Log.i(TAG, "REWIND released: emuAtRewindStart=${emuAtRewindStart / 1000}ms " +
                            "resumedAt=${after / 1000}ms continued=${after > 0}")
                    }
                }
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

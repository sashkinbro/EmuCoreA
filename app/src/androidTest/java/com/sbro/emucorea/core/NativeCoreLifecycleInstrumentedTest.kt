// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreA-Proprietary
package com.sbro.emucorea.core

import android.content.Intent
import android.graphics.PixelFormat
import android.graphics.Bitmap
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.os.Bundle
import android.util.Base64
import android.util.Log
import android.view.PixelCopy
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.FrameLayout
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.sbro.emucorea.RuntimeTestActivity
import java.io.File
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Callable
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/** Real JNI, GPU and device-audio coverage; the guest is generated locally. */
@RunWith(AndroidJUnit4::class)
class NativeCoreLifecycleInstrumentedTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val context get() = instrumentation.targetContext
    private lateinit var executor: ExecutorService
    private lateinit var activity: RuntimeTestActivity
    private lateinit var root: File
    private lateinit var guest: File
    @Volatile private var publishedSurface: HostSurface? = null
    private lateinit var surfaceView: SurfaceView
    private lateinit var surfaceContainer: FrameLayout

    private data class HostSurface(val surface: Surface, val width: Int, val height: Int)

    @Before
    fun setUp() {
        assertTrue("Native library must load on the arm64 device", NativePpsspp.ensureLoaded())
        executor = Executors.newSingleThreadExecutor { work -> Thread(work, "NativeLifecycleTest-Frame") }
        // Application initialization leaves the native singleton idle. This
        // suite owns it directly and never starts CoreRuntime's worker.
        onFrame { NativePpsspp.nativeShutdown() }
        val checkpoint = InstrumentationRegistry.getArguments().getString("navigationCheckpoint") == "true"
        root = File(context.cacheDir, if (checkpoint) "native-lifecycle-gameplay-checkpoint" else "native-lifecycle-${System.nanoTime()}").apply { mkdirs() }
        guest = File(root, "lifecycle.elf").apply { writeBytes(minimalPspElf()) }
        activity = instrumentation.startActivitySync(
            Intent(context, RuntimeTestActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        ) as RuntimeTestActivity
        createSurface(480, 272)
    }

    @After
    fun tearDown() {
        try {
            if (::executor.isInitialized && !executor.isShutdown) onFrame { NativePpsspp.nativeShutdown() }
        } finally {
            if (::executor.isInitialized) executor.shutdownNow()
            if (::activity.isInitialized) {
                instrumentation.runOnMainSync { activity.finish() }
                instrumentation.waitForIdleSync()
            }
            val arguments = InstrumentationRegistry.getArguments()
            if (::root.isInitialized && (arguments.getString("navigationCheckpoint") != "true" ||
                arguments.getString("cleanupCheckpoint") == "true")) root.deleteRecursively()
        }
    }

    @Test(timeout = 180_000)
    fun everyRendererRetainsBootOptionsAcrossResizeDetachAndReboot() {
        for (renderer in listOf(0, 1, 2)) {
            for ((cpuOption, expectedCpu) in listOf("IR JIT" to 2, "JIT" to 1)) {
                initialize(renderer)
                onFrame {
                    NativePpsspp.nativeSetConfig("ppsspp_cpu_core", cpuOption)
                    NativePpsspp.nativeSetConfig("fps_limit", "45")
                    NativePpsspp.nativeSetConfig("ppsspp_texture_replacement", "enabled")
                    assertTrue("Boot request renderer=$renderer cpu=$cpuOption", NativePpsspp.nativeBoot(guest.path))
                }
                awaitBoot()
                onFrame {
                    val current = state()
                    assertEquals("Renderer must honor the request", renderer, NativePpsspp.nativeGetActiveRenderer())
                    assertEquals("Boot must preserve selected CPU", expectedCpu, current[CPU])
                    assertEquals("Boot must preserve custom frame limit mode", 1, current[FPS_MODE])
                    assertEquals(45, current[TARGET_FPS])
                    assertEquals(if (renderer == 0) 1 else 0, current[SOFTWARE])
                    assertEquals("Loader must retain texture replacement override", 1, current[REPLACE_TEXTURES])
                    assertFrameProgress()
                }

                // Exercise a real Android buffer resize before notifying JNI.
                instrumentation.runOnMainSync { surfaceView.holder.setFixedSize(640, 360) }
                val resized = awaitSurface(640, 360)
                onFrame {
                    assertTrue(NativePpsspp.nativeSetSurface(resized.surface, resized.width, resized.height))
                    assertEquals(640, state()[WIDTH])
                    assertEquals(360, state()[HEIGHT])
                    assertFrameProgress()
                    NativePpsspp.nativeSetPaused(true)
                    assertEquals(0, state()[AUDIO])
                    NativePpsspp.nativeSetSurface(null, 0, 0)
                }

                // Destroy the actual SurfaceView, not only the native binding.
                createSurface(480, 272)
                val replacement = awaitSurface(480, 272)
                onFrame {
                    assertTrue(NativePpsspp.nativeSetSurface(replacement.surface, 480, 272))
                    NativePpsspp.nativeSetPaused(false)
                    assertEquals(1, state()[AUDIO])
                    assertFrameProgress()
                    assertTrue(NativePpsspp.nativeGetRuntimeError().isEmpty())
                    NativePpsspp.nativeShutdown()
                    assertEquals(0, state()[BOOTED])
                    assertEquals(0, state()[AUDIO])
                }
            }
        }
    }

    @Test(timeout = 90_000)
    fun queuedBootPauseAndLiveOutputChangesRespectPauseState() {
        initialize(1)
        onFrame {
            assertTrue(NativePpsspp.nativeBoot(guest.path))
            assertEquals("No output may consume while the loader initializes audio", 0, state()[AUDIO])
            NativePpsspp.nativeSetPaused(true)
        }
        awaitBoot()
        onFrame {
            assertEquals("Boot completion must retain an existing pause", 0, state()[AUDIO])
            NativePpsspp.nativeSetConfig("audio_backend", "opensl")
            NativePpsspp.nativeSetConfig("audio_low_latency", "1")
            NativePpsspp.nativeSetConfig("audio_buffer_ms", "20")
            assertEquals("Changing output settings must not resume paused output", 0, state()[AUDIO])
            NativePpsspp.nativeSetPaused(false)
            assertEquals("Resume starts selected output", 1, state()[AUDIO])
            assertFrameProgress()
            val save = File(root, "audio.state")
            assertTrue(NativePpsspp.nativeSaveState(save.path))
            assertTrue(NativePpsspp.nativeLoadState(save.path))
            assertEquals("An active state load restores output", 1, state()[AUDIO])
            NativePpsspp.nativeSetPaused(true)
            assertEquals(0, state()[AUDIO])
            assertTrue(NativePpsspp.nativeLoadState(save.path))
            assertEquals("A state load must not resume paused output", 0, state()[AUDIO])
            NativePpsspp.nativeSetConfig("audio_backend", "aaudio")
            NativePpsspp.nativeSetConfig("audio_low_latency", "0")
            NativePpsspp.nativeSetConfig("audio_buffer_ms", "0")
            assertEquals(0, state()[AUDIO])
            NativePpsspp.nativeSetPaused(false)
            assertEquals(1, state()[AUDIO])
            assertFrameProgress()
        }
    }

    @Test(timeout = 60_000)
    fun invalidBootPublishesAnErrorAndNextSessionCanBoot() {
        initialize(1)
        val broken = File(root, "truncated.elf").apply { writeBytes(minimalPspElf().copyOf(64)) }
        onFrame { assertTrue(NativePpsspp.nativeBoot(broken.path)) }
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(20)
        var error = ""
        while (error.isEmpty() && System.nanoTime() < deadline) {
            error = onFrame {
                NativePpsspp.nativeRunFrame()
                NativePpsspp.nativeGetRuntimeError()
            }
            if (error.isEmpty()) Thread.sleep(10)
        }
        assertTrue("Asynchronous loader failure must reach the frontend", error.isNotBlank())
        onFrame {
            assertFalse(NativePpsspp.nativeIsBooted())
            assertEquals("Failed boot must not leave output running", 0, state()[AUDIO])
            NativePpsspp.nativeShutdown()
        }
        initialize(1)
        onFrame { assertTrue(NativePpsspp.nativeBoot(guest.path)) }
        awaitBoot()
        onFrame {
            assertTrue("A new session clears the previous boot failure", NativePpsspp.nativeGetRuntimeError().isEmpty())
            assertFrameProgress()
        }
    }

    @Test(timeout = 45_000)
    fun invalidGuestProgramCounterReportsRuntimeFailureAndStopsAudio() {
        initialize(1)
        val faultBytes = minimalPspElf().also {
            ByteBuffer.wrap(it).order(ByteOrder.LITTLE_ENDIAN)
                .putInt(0x100, 0x08000000 or ((0x0F000000 ushr 2) and 0x03FFFFFF))
        }
        val faultGuest = File(root, "invalid-pc.elf").apply { writeBytes(faultBytes) }
        onFrame {
            NativePpsspp.nativeSetConfig("ppsspp_cpu_core", "Interpreter")
            assertTrue(NativePpsspp.nativeBoot(faultGuest.path))
        }
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        var error = ""
        while (error.isEmpty() && System.nanoTime() < deadline) {
            error = onFrame {
                NativePpsspp.nativeRunFrame()
                NativePpsspp.nativeGetRuntimeError()
            }
            if (error.isEmpty()) Thread.sleep(10)
        }
        assertTrue("Invalid guest PC must report a frontend runtime failure", error.isNotBlank())
        onFrame {
            assertEquals("Guest execution failure must stop audio", 0, state()[AUDIO])
            assertEquals(0L, NativePpsspp.nativeRunFrame())
        }
    }

    @Test(timeout = 60_000)
    fun changingCpuCoreDuringPlayUpdatesTheRunningCpu() {
        initialize(1)
        onFrame {
            NativePpsspp.nativeSetConfig("ppsspp_cpu_core", "JIT")
            assertTrue(NativePpsspp.nativeBoot(guest.path))
        }
        awaitBoot()
        onFrame {
            assertEquals(1, state()[CPU])
            NativePpsspp.nativeSetConfig("ppsspp_cpu_core", "Interpreter")
            NativePpsspp.nativeRunFrame()
            assertEquals("A live CPU setting must update the CPU that executes the guest", 0, state()[CPU])
            assertFrameProgress()
            NativePpsspp.nativeSetConfig("ppsspp_cpu_core", "JIT")
            NativePpsspp.nativeRunFrame()
            assertEquals(1, state()[CPU])
            assertFrameProgress()
        }
    }

    @Test(timeout = 120_000)
    fun earlyDetachAndDirectSurfaceReplacementRecoverAudioAndFrames() {
        // Extra ELF bytes keep the asynchronous loader busy long enough to
        // exercise detach immediately after its first frame-thread poll.
        val slowGuest = File(root, "pending.elf").apply {
            writeBytes(minimalPspElf().copyOf(8 * 1024 * 1024))
        }
        for (renderer in listOf(0, 1, 2)) {
            initialize(renderer)
            onFrame {
                assertTrue(NativePpsspp.nativeBoot(slowGuest.path))
                NativePpsspp.nativeRunFrame()
                NativePpsspp.nativeSetSurface(null, 0, 0)
                assertEquals("Detaching during startup must stop the device consumer", 0, state()[AUDIO])
            }
            createSurface(480, 272)
            onFrame {
                val replacement = checkNotNull(publishedSurface)
                assertTrue(NativePpsspp.nativeSetSurface(replacement.surface, 480, 272))
            }
            awaitBoot()
            onFrame {
                assertFrameProgress()
                assertEquals("An unpaused core must restore audio after startup surface loss", 1, state()[AUDIO])
            }

            // Keep the first native window valid while handing the core a
            // different non-null window. There is no intervening detach JNI.
            val previous = checkNotNull(publishedSurface).surface
            createSurface(640, 360, keepExisting = true)
            val direct = checkNotNull(publishedSurface)
            assertTrue("Old window must still be alive during direct replacement", previous.isValid)
            onFrame {
                assertTrue(NativePpsspp.nativeSetSurface(direct.surface, 640, 360))
                assertFrameProgress()
                assertEquals(640, state()[WIDTH])
                assertEquals(360, state()[HEIGHT])
                assertEquals(1, state()[AUDIO])
                assertTrue(NativePpsspp.nativeGetRuntimeError().isEmpty())
                NativePpsspp.nativeShutdown()
            }
        }
    }

    @Test(timeout = 90_000)
    fun activeAudioRebuildsAcrossBackendAndLatencyChanges() {
        initialize(1)
        onFrame { assertTrue(NativePpsspp.nativeBoot(guest.path)) }
        awaitBoot()
        onFrame {
            for (backend in listOf("opensl", "aaudio", "opensl", "aaudio")) {
                NativePpsspp.nativeSetConfig("audio_backend", backend)
                NativePpsspp.nativeSetConfig("audio_buffer_ms", "500")
                assertEquals("Active $backend rebuild must restart output", 1, state()[AUDIO])
                assertFrameProgress()
                NativePpsspp.nativeSetConfig("audio_low_latency", "1")
                NativePpsspp.nativeSetConfig("audio_buffer_ms", "10")
                assertEquals(1, state()[AUDIO])
                assertFrameProgress()
                NativePpsspp.nativeSetConfig("audio_low_latency", "0")
                NativePpsspp.nativeSetConfig("audio_buffer_ms", "0")
                assertEquals(1, state()[AUDIO])
                assertFrameProgress()
                assertTrue(NativePpsspp.nativeGetRuntimeError().isEmpty())
            }
        }
    }

    @Test(timeout = 60_000)
    fun abandonedWindowReportsFailureAndFreshSessionRecovers() {
        val onlyRenderer = InstrumentationRegistry.getArguments().getString("abandonedRenderer")?.toIntOrNull()
        for (renderer in onlyRenderer?.let { listOf(it) } ?: listOf(1, 2)) {
            initialize(renderer)
            onFrame { assertTrue(NativePpsspp.nativeBoot(guest.path)) }
            awaitBoot()
            val abandoned = checkNotNull(publishedSurface).surface
            // Model a surface disappearing before its detach event reaches the
            // frame worker. The newly published window is deliberately not bound.
            createSurface(480, 272)
            assertFalse("Removed Android surface must be abandoned", abandoned.isValid)
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10)
            var error = ""
            while (error.isEmpty() && System.nanoTime() < deadline) {
                error = onFrame {
                    NativePpsspp.nativeRunFrame()
                    NativePpsspp.nativeGetRuntimeError()
                }
                if (error.isEmpty()) Thread.sleep(10)
            }
            assertTrue("An abandoned native window must report a recoverable frontend failure", error.isNotBlank())
            onFrame {
                assertEquals("Frames stop after presentation failure", 0L, NativePpsspp.nativeRunFrame())
                assertEquals("A failed surface must stop its audio consumer", 0, state()[AUDIO])
                NativePpsspp.nativeShutdown()
            }
            initialize(renderer)
            onFrame { assertTrue(NativePpsspp.nativeBoot(guest.path)) }
            awaitBoot()
            onFrame {
                assertFrameProgress()
                NativePpsspp.nativeShutdown()
            }
        }
    }

    @Test(timeout = 120_000)
    fun pausedRuntimeRendererRestartKeepsPauseAndSessionCpuOverride() {
        CoreRuntime.shutdown()
        try {
            CoreRuntime.initialize(context, File(root, "runtime-memstick").apply { mkdirs() }.path)
            assertTrue(CoreRuntime.updateSetting("EmuCoreA/GS", "Renderer", "14"))
            val surface = checkNotNull(publishedSurface)
            CoreRuntime.attachSurface(surface.surface, surface.width, surface.height)
            assertTrue(CoreRuntime.start(guest.path, mapOf("ppsspp_cpu_core" to "IR JIT")))
            awaitRuntimeBoot()
            CoreRuntime.pause()
            assertEquals(0, state()[AUDIO])
            assertTrue(CoreRuntime.restartWithRenderer(RendererDefaults.SOFTWARE))
            assertEquals("A second renderer choice can arrive before the first boot restores", 0, state()[AUDIO])
            assertTrue(CoreRuntime.restartWithRenderer(RendererDefaults.OPENGL))
            awaitRuntimeBoot(expectPaused = true)
            assertEquals(RendererDefaults.CORE_OPENGL, NativePpsspp.nativeGetActiveRenderer())
            assertEquals(2, state()[CPU])
            Thread.sleep(100)
            assertEquals("Renderer restart must preserve the menu pause after restoring state", 0, state()[AUDIO])
            CoreRuntime.resume()
            assertEquals(1, state()[AUDIO])
        } finally {
            CoreRuntime.shutdown()
        }
    }

    @Test(timeout = 60_000)
    fun guestExitStopsAudioAndAllowsAnotherSession() {
        initialize(1)
        val exitingGuest = File(root, "exit.elf").apply { writeBytes(minimalPspElf(exitImmediately = true)) }
        onFrame { assertTrue(NativePpsspp.nativeBoot(exitingGuest.path)) }
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(20)
        var exited = false
        while (!exited && System.nanoTime() < deadline) {
            exited = onFrame {
                val time = NativePpsspp.nativeRunFrame()
                assertTrue("Exit fixture failed to boot: ${NativePpsspp.nativeGetRuntimeError()}", NativePpsspp.nativeGetRuntimeError().isEmpty())
                time == -1L
            }
            if (!exited) Thread.sleep(10)
        }
        assertTrue("Imported sceKernelExitGame must stop the guest", exited)
        onFrame {
            assertEquals("A powered-down guest must not leave device callbacks running", 0, state()[AUDIO])
            NativePpsspp.nativeShutdown()
        }
        initialize(1)
        onFrame { assertTrue(NativePpsspp.nativeBoot(guest.path)) }
        awaitBoot()
        onFrame { assertFrameProgress() }
    }

    @Test(timeout = 60_000)
    fun immediateRewindReleaseWaitsForQueuedRestoreBeforeResumingAudio() {
        initialize(1)
        onFrame {
            NativePpsspp.nativeSetConfig("fps_limit", "60")
            NativePpsspp.nativeSetRewindEnabled(true)
            assertTrue(NativePpsspp.nativeBoot(guest.path))
        }
        awaitBoot()
        onFrame {
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(12)
            var queued = false
            while (!queued && System.nanoTime() < deadline) {
                NativePpsspp.nativeRunFrame()
                queued = NativePpsspp.nativeRewindStep()
            }
            assertTrue("The running guest must produce a rewind snapshot", queued)
            assertEquals(0, state()[AUDIO])
            // Releasing between queueing and the next frame is legal input;
            // the pending restore still has to clear the ring without output.
            NativePpsspp.nativeRewindRelease()
            assertEquals("Queued rewind must retain audio suspension after early release", 0, state()[AUDIO])
            NativePpsspp.nativeRunFrame()
            assertEquals("Output resumes once the released rewind has actually restored", 1, state()[AUDIO])
            assertFrameProgress()
            NativePpsspp.nativeSetRewindEnabled(false)
        }
    }

    @Test(timeout = 120_000)
    fun generatedDisplayGuestPresentsKnownPixelsAcrossEveryRenderer() {
        val displayGuest = File(root, "display.elf").apply { writeBytes(displayPspElf()) }
        val identity = writeIdentityPreset()
        for (renderer in listOf(1, 2, 0)) {
            initialize(renderer)
            onFrame { assertTrue(NativePpsspp.nativeBoot(displayGuest.path)) }
            awaitBoot()
            for (stage in listOf("off", "identity", "off-again")) {
                onFrame {
                    NativePpsspp.nativeSetShaderPreset(if (stage == "identity") identity.path else "")
                    repeat(60) { NativePpsspp.nativeRunFrame() }
                    assertTrue(NativePpsspp.nativeGetRuntimeError().isEmpty())
                }
                val screenshot = copySurface(checkNotNull(publishedSurface))
                val center = screenshot.getPixel(screenshot.width / 2, screenshot.height / 2)
                screenshot.recycle()
                Log.i("NativeLifecycleSmoke", "display renderer=$renderer shader=$stage center=${Integer.toHexString(center)}")
                assertTrue("Known PSP framebuffer red renderer=$renderer shader=$stage center=${Integer.toHexString(center)}",
                    kotlin.math.abs(((center ushr 16) and 255) - 0xCC) <= 8)
                assertTrue("Known PSP framebuffer green renderer=$renderer shader=$stage", kotlin.math.abs(((center ushr 8) and 255) - 0x66) <= 8)
                assertTrue("Known PSP framebuffer blue renderer=$renderer shader=$stage", kotlin.math.abs((center and 255) - 0x33) <= 8)
            }
            onFrame { NativePpsspp.nativeShutdown() }
        }
    }

    @Test(timeout = 120_000)
    fun generatedGeGuestPresentsInBufferedAndDirectHardwareModes() {
        val geGuest = File(root, "ge-display.elf").apply { writeBytes(PspGeDisplayFixture.elf()) }
        val missingOutput = mutableListOf<String>()
        for (direct in listOf(false, true)) {
            for (renderer in listOf(1, 2, 0)) {
                if (direct && renderer == 0) continue
                initialize(renderer)
                onFrame {
                    NativePpsspp.nativeSetConfig("ppsspp_skip_buffer_effects", if (direct) "enabled" else "disabled")
                    NativePpsspp.nativeSetConfig("fps_limit", "60")
                    assertTrue(NativePpsspp.nativeBoot(geGuest.path))
                }
                awaitBoot()
                onFrame {
                    repeat(60) { NativePpsspp.nativeRunFrame() }
                    assertTrue(NativePpsspp.nativeGetRuntimeError().isEmpty())
                }
                val screenshot = copySurface(checkNotNull(publishedSurface))
                val center = screenshot.getPixel(screenshot.width / 2, screenshot.height / 2)
                screenshot.setPremultiplied(false)
                val rawCenter = screenshot.getPixel(screenshot.width / 2, screenshot.height / 2)
                screenshot.recycle()
                val composited = copyCompositedSurface()
                val visibleCenter = composited.getPixel(composited.width / 2, composited.height / 2)
                composited.recycle()
                val result = "GE renderer=$renderer direct=$direct center=${Integer.toHexString(center)}" +
                    " raw=${Integer.toHexString(rawCenter)} visible=${Integer.toHexString(visibleCenter)}"
                Log.i("NativeLifecycleSmoke", result)
                instrumentation.sendStatus(0, Bundle().apply { putString("nativeGeResult", result) })
                if (kotlin.math.abs(((visibleCenter ushr 16) and 255) - 0xCC) > 8 ||
                    kotlin.math.abs(((visibleCenter ushr 8) and 255) - 0x66) > 8 ||
                    kotlin.math.abs((visibleCenter and 255) - 0x33) > 8) missingOutput += result
                onFrame { NativePpsspp.nativeShutdown() }
            }
        }
        assertTrue("GE framebuffer must present expected RGB CC6633: ${missingOutput.joinToString()}", missingOutput.isEmpty())
    }

    /** Opt-in real content smoke: adb am instrument -e game <path-or-document-URI>. */
    @Test(timeout = 300_000)
    fun optionalRealGameBootsAndPresentsAcrossEveryRenderer() {
        val game = InstrumentationRegistry.getArguments().getString("game")
        assumeTrue("Supply the game instrumentation argument to run the real-game smoke", !game.isNullOrBlank())
        val startupSeconds = InstrumentationRegistry.getArguments().getString("startupSeconds")
            ?.toLongOrNull()?.coerceIn(1, 60) ?: 30L
        val requestedRenderer = InstrumentationRegistry.getArguments().getString("gameRenderer")?.toIntOrNull()
        val renderers = requestedRenderer?.let {
            require(it in 0..2) { "gameRenderer must be 0, 1 or 2" }
            listOf(it)
        } ?: listOf(1, 2, 0)
        val outputDirectory = File(context.getExternalFilesDir(null), "native-lifecycle-smoke").apply { mkdirs() }
        fun saveCapture(name: String, bitmap: Bitmap) {
            val bytes = ByteArrayOutputStream().use {
                assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it))
                it.toByteArray()
            }
            File(outputDirectory, name).writeBytes(bytes)
            if (InstrumentationRegistry.getArguments().getString("captureBase64") == "true") {
                val encoded = Base64.encodeToString(bytes, Base64.NO_WRAP)
                if (encoded.length <= 512_000) {
                    instrumentation.sendStatus(0, Bundle().apply {
                        putString("nativeScreenshotName", name)
                        putString("nativeScreenshotPng", encoded)
                    })
                } else {
                    val chunks = encoded.chunked(256_000)
                    for ((index, chunk) in chunks.withIndex()) instrumentation.sendStatus(0, Bundle().apply {
                        putString("nativeScreenshotChunkName", name)
                        putInt("nativeScreenshotChunkIndex", index)
                        putInt("nativeScreenshotChunkCount", chunks.size)
                        putString("nativeScreenshotChunk", chunk)
                    })
                }
            }
        }
        val identity = writeIdentityPreset()
        val metrics = mutableListOf<String>()
        val missingOutput = mutableListOf<String>()
        val navigationState = File(root, "game-navigation.state")
        val reuseNavigationState = InstrumentationRegistry.getArguments().getString("reuseNavigationState") == "true"
        for (renderer in renderers) {
            initialize(renderer)
            val boundSurface = checkNotNull(publishedSurface)
            val descriptor = if (game!!.startsWith("content://")) {
                checkNotNull(context.contentResolver.openFileDescriptor(Uri.parse(game), "r"))
            } else null
            try {
                onFrame {
                    NativePpsspp.nativeSetConfig("ppsspp_cpu_core", "JIT")
                    NativePpsspp.nativeSetConfig("ppsspp_texture_replacement", "disabled")
                    NativePpsspp.nativeSetConfig("fps_limit", "60")
                    assertTrue(if (descriptor != null) {
                        NativePpsspp.nativeBootFd(descriptor.fd, "real-game.iso")
                    } else NativePpsspp.nativeBoot(game))
                }
                awaitBoot()
                val actions = InstrumentationRegistry.getArguments().getString("gameActions")
                val checkpoint = InstrumentationRegistry.getArguments().getString("navigationCheckpoint") == "true"
                if ((reuseNavigationState || checkpoint) && navigationState.isFile) {
                    onFrame {
                        assertTrue("Load the same navigated scene on the next renderer", NativePpsspp.nativeLoadState(navigationState.path))
                        assertFrameProgress()
                    }
                }
                if (!actions.isNullOrBlank() && !(reuseNavigationState && renderer != renderers.first())) {
                    fun runGuestFor(microseconds: Long) {
                        val begin = onFrame { NativePpsspp.nativeRunFrame() }
                        var end = begin
                        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30)
                        while (end - begin < microseconds && System.nanoTime() < deadline) {
                            end = onFrame {
                                val time = NativePpsspp.nativeRunFrame()
                                assertTrue("Gameplay navigation failed: ${NativePpsspp.nativeGetRuntimeError()}", NativePpsspp.nativeGetRuntimeError().isEmpty())
                                assertTrue("Guest exited during gameplay navigation", time >= 0)
                                time
                            }
                        }
                        assertTrue("Gameplay navigation must advance", end > begin)
                    }
                    val buttons = mapOf("cross" to 0x4000, "circle" to 0x2000, "square" to 0x8000,
                        "triangle" to 0x1000, "start" to 0x0008, "up" to 0x0010,
                        "down" to 0x0040, "left" to 0x0080, "right" to 0x0020)
                    for ((index, action) in actions.split(',').withIndex()) {
                        val parts = action.trim().split(':')
                        val button = parts[0]
                        val seconds = parts.getOrNull(1)?.toDoubleOrNull()?.coerceIn(0.25, 20.0) ?: 2.0
                        if (button != "wait") {
                            val mask = requireNotNull(buttons[button]) { "Unknown game action: $button" }
                            onFrame { NativePpsspp.nativeSetPadButtons(0, mask.inv() and 0xFFFF) }
                            runGuestFor(200_000)
                            onFrame { NativePpsspp.nativeSetPadButtons(0, 0xFFFF) }
                        }
                        runGuestFor((seconds * 1_000_000).toLong())
                        onFrame { NativePpsspp.nativeSetPaused(true) }
                        val visible = copyCompositedSurface()
                        saveCapture("renderer-$renderer-navigation-$index-$button.png", visible)
                        visible.recycle()
                        Log.i("NativeLifecycleSmoke", "navigation renderer=$renderer index=$index action=$action")
                        instrumentation.sendStatus(0, Bundle().apply {
                            putString("nativeNavigationStep", "renderer=$renderer index=$index action=$action")
                        })
                        onFrame { NativePpsspp.nativeSetPaused(false) }
                    }
                    if (reuseNavigationState || checkpoint) onFrame {
                        assertTrue("Save the navigated scene for renderer comparisons", NativePpsspp.nativeSaveState(navigationState.path))
                    }
                }
                for (stage in listOf("off", "identity", "off-again")) {
                    onFrame { NativePpsspp.nativeSetPaused(false) }
                    if (InstrumentationRegistry.getArguments().getString("reloadNavigationEachStage") == "true") onFrame {
                        assertTrue("Reload the verified scene before each shader comparison", NativePpsspp.nativeLoadState(navigationState.path))
                        assertFrameProgress()
                    }
                    onFrame { NativePpsspp.nativeSetShaderPreset(if (stage == "identity") identity.path else "") }
                    val expectMovingScene = InstrumentationRegistry.getArguments().getString("expectMovingScene") == "true"
                    val gameplayInput = InstrumentationRegistry.getArguments().getString("gameplayInput") == "true"
                    val startPixelHash = if (expectMovingScene) {
                        onFrame { NativePpsspp.nativeSetPaused(true) }
                        val beforeVisible = copyCompositedSurface()
                        saveCapture("renderer-$renderer-$stage-before.png", beforeVisible)
                        beforeVisible.recycle()
                        val before = copySurface(boundSurface)
                        before.setPremultiplied(false)
                        val values = IntArray(before.width * before.height)
                        before.getPixels(values, 0, before.width, 0, 0, before.width, before.height)
                        before.recycle()
                        values.fold(1) { hash, pixel -> 31 * hash + (pixel and 0xFFFFFF) }
                    } else 0
                    onFrame { NativePpsspp.nativeSetPaused(false) }
                    val wallStart = System.nanoTime()
                    val emulatedStart = onFrame { NativePpsspp.nativeRunFrame() }
                    var emulatedEnd = emulatedStart
                    var frames = 0
                    val stageSeconds = InstrumentationRegistry.getArguments().getString("stageSeconds")?.toLongOrNull()?.coerceIn(1, 30)
                        ?: if (stage == "off") startupSeconds else 1L
                    val targetUs = stageSeconds * 1_000_000L
                    val wallLimit = stageSeconds + 20L
                    while (emulatedEnd - emulatedStart < targetUs &&
                        System.nanoTime() - wallStart < TimeUnit.SECONDS.toNanos(wallLimit)) {
                        emulatedEnd = onFrame {
                            assertTrue("Real-game fixture surface must remain alive", boundSurface.surface.isValid)
                            if (gameplayInput) {
                                val button = intArrayOf(0x8000, 0x1000, 0x4000, 0x2000, 0x0020)[(frames / 30) % 5]
                                NativePpsspp.nativeSetPadButtons(0, if (frames % 30 < 10) button.inv() and 0xFFFF else 0xFFFF)
                            }
                            val time = NativePpsspp.nativeRunFrame()
                            assertTrue("Real-game native error: ${NativePpsspp.nativeGetRuntimeError()}", NativePpsspp.nativeGetRuntimeError().isEmpty())
                            assertTrue("Real game must not exit during startup", time >= 0)
                            time
                        }
                        frames++
                    }
                    if (gameplayInput) onFrame { NativePpsspp.nativeSetPadButtons(0, 0xFFFF) }
                    assertTrue("Real guest must advance renderer=$renderer shader=$stage", emulatedEnd > emulatedStart)
                    val wallSeconds = (System.nanoTime() - wallStart) / 1e9
                    val emulatedSeconds = (emulatedEnd - emulatedStart) / 1e6
                    val displayStats = onFrame { NativePpsspp.nativeGetDisplayStats() }
                    val runtimeState = onFrame { state().joinToString(prefix = "[", postfix = "]") }
                    val frameSize = onFrame { NativePpsspp.nativeGetFrameSize()?.joinToString(prefix = "[", postfix = "]") ?: "[]" }
                    // Capture/transport can take seconds on a real device.
                    // Stop the callback while no guest frames are produced.
                    onFrame { NativePpsspp.nativeSetPaused(true) }
                    val screenshot = copySurface(boundSurface)
                    // PSP alpha stores stencil and can be zero even though
                    // the opaque Android surface visibly presents its RGB.
                    screenshot.setPremultiplied(false)
                    val pixels = IntArray(screenshot.width * screenshot.height)
                    screenshot.getPixels(pixels, 0, screenshot.width, 0, 0, screenshot.width, screenshot.height)
                    val nonBlack = pixels.count { (it and 0x00FFFFFF) != 0 }
                    val endPixelHash = pixels.fold(1) { hash, pixel -> 31 * hash + (pixel and 0xFFFFFF) }
                    for (index in pixels.indices) pixels[index] = pixels[index] or 0xFF000000.toInt()
                    val surfaceRgb = Bitmap.createBitmap(pixels, screenshot.width, screenshot.height, Bitmap.Config.ARGB_8888)
                    screenshot.recycle()
                    saveCapture("renderer-$renderer-$stage-surface-rgb.png", surfaceRgb)
                    surfaceRgb.recycle()
                    val visible = copyCompositedSurface()
                    val visiblePixels = IntArray(visible.width * visible.height)
                    visible.getPixels(visiblePixels, 0, visible.width, 0, 0, visible.width, visible.height)
                    val visibleNonBlack = visiblePixels.count { (it and 0x00FFFFFF) != 0 }
                    saveCapture("renderer-$renderer-$stage.png", visible)
                    visible.recycle()
                    val metric = "{\"renderer\":$renderer,\"shader\":\"$stage\",\"frames\":$frames,\"wallSeconds\":$wallSeconds," +
                        "\"emulatedSeconds\":$emulatedSeconds,\"speedPercent\":${emulatedSeconds / wallSeconds * 100}," +
                        "\"nativeFramesPerSecond\":${frames / wallSeconds},\"vps\":${displayStats?.getOrNull(0) ?: 0f}," +
                        "\"flipsPerSecond\":${displayStats?.getOrNull(1) ?: 0f},\"actualFps\":${displayStats?.getOrNull(2) ?: 0f}," +
                        "\"nonBlackPixels\":$nonBlack,\"visibleNonBlackPixels\":$visibleNonBlack," +
                        "\"startPixelHash\":$startPixelHash,\"endPixelHash\":$endPixelHash," +
                        "\"width\":${boundSurface.width},\"height\":${boundSurface.height}," +
                        "\"runtime\":$runtimeState,\"frameSize\":$frameSize}"
                    metrics += metric
                    File(outputDirectory, "metrics.json").writeText(metrics.joinToString(prefix = "[", postfix = "]"))
                    Log.i("NativeLifecycleSmoke", metric)
                    instrumentation.sendStatus(0, Bundle().apply { putString("nativeSmokeMetric", metric) })
                    if (nonBlack <= 32 || visibleNonBlack <= 32) missingOutput += "renderer=$renderer shader=$stage"
                    if (expectMovingScene && startPixelHash == endPixelHash) missingOutput += "renderer=$renderer shader=$stage frozen-scene"
                }
            } finally {
                onFrame { NativePpsspp.nativeShutdown() }
                if (InstrumentationRegistry.getArguments().getString("captureAudio") == "true") {
                    val captured = File(root, "audio-capture.wav")
                    assertTrue("Debug audio capture must be enabled before nativeInit", captured.isFile)
                    val allBytes = captured.readBytes()
                    val header = ByteBuffer.wrap(allBytes).order(ByteOrder.LITTLE_ENDIAN)
                    val rate = header.getInt(24)
                    val channels = header.getShort(22).toInt()
                    val length = minOf(allBytes.size - 44, rate * channels * 2 * 10)
                    val bytes = allBytes.copyOf(44 + length)
                    allBytes.copyInto(bytes, 44, allBytes.size - length, allBytes.size)
                    ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).apply {
                        putInt(4, 36 + length)
                        putInt(40, length)
                    }
                    var peak = 0
                    var nonzero = 0
                    var energy = 0.0
                    val samples = ByteBuffer.wrap(bytes, 44, length).order(ByteOrder.LITTLE_ENDIAN)
                    while (samples.remaining() >= 2) {
                        val sample = samples.short.toInt()
                        peak = maxOf(peak, kotlin.math.abs(sample))
                        if (sample != 0) nonzero++
                        energy += sample.toDouble() * sample
                    }
                    val sampleCount = length / 2
                    val audioMetric = "{\"renderer\":$renderer,\"sampleRate\":$rate,\"channels\":$channels," +
                        "\"seconds\":${sampleCount.toDouble() / rate / channels},\"peak\":$peak," +
                        "\"nonzeroSamples\":$nonzero,\"rms\":${kotlin.math.sqrt(energy / sampleCount)}}"
                    val name = "renderer-$renderer-audio.wav"
                    File(outputDirectory, name).writeBytes(bytes)
                    val chunks = Base64.encodeToString(bytes, Base64.NO_WRAP).chunked(256_000)
                    for ((index, chunk) in chunks.withIndex()) instrumentation.sendStatus(0, Bundle().apply {
                        putString("nativeAudioName", name)
                        putInt("nativeAudioIndex", index)
                        putInt("nativeAudioCount", chunks.size)
                        putString("nativeAudioChunk", chunk)
                    })
                    Log.i("NativeLifecycleSmoke", "audio $audioMetric")
                    instrumentation.sendStatus(0, Bundle().apply { putString("nativeAudioMetric", audioMetric) })
                    assertTrue("Captured game output must contain non-silent PCM", peak > 0 && nonzero > 0)
                }
                descriptor?.close()
            }
        }
        assertTrue("Every renderer and shader stage must present game pixels: ${missingOutput.joinToString()}", missingOutput.isEmpty())
    }

    private fun writeIdentityPreset(): File {
        val directory = File(root, "identity").apply { mkdirs() }
        File(directory, "identity.slang").writeText("""
            #version 450
            layout(push_constant) uniform Push { mat4 MVP; vec4 OutputSize; vec4 OriginalSize; vec4 SourceSize; uint FrameCount; } params;
            #pragma stage vertex
            layout(location=0) in vec4 Position;
            layout(location=1) in vec2 TexCoord;
            layout(location=0) out vec2 vTexCoord;
            void main() { gl_Position = params.MVP * Position; vTexCoord = TexCoord; }
            #pragma stage fragment
            layout(location=0) in vec2 vTexCoord;
            layout(location=0) out vec4 FragColor;
            layout(set=0,binding=2) uniform sampler2D Source;
            void main() { FragColor = texture(Source, vTexCoord); }
        """.trimIndent() + "\n")
        return File(directory, "identity.slangp").apply {
            writeText("shaders = 1\nshader0 = identity.slang\nfilter_linear0 = false\n")
        }
    }

    private fun awaitRuntimeBoot(expectPaused: Boolean = false) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(20)
        while (System.nanoTime() < deadline) {
            assertTrue("CoreRuntime failed: ${CoreRuntime.failure.value}", CoreRuntime.failure.value == null)
            if (expectPaused) assertEquals("Audio must stay paused throughout renderer restart", 0, state()[AUDIO])
            if (NativePpsspp.nativeIsBooted()) return
            Thread.sleep(10)
        }
        throw AssertionError("CoreRuntime worker did not finish booting in 20 seconds")
    }

    private fun copySurface(surface: HostSurface): Bitmap {
        val bitmap = Bitmap.createBitmap(surface.width, surface.height, Bitmap.Config.ARGB_8888)
        val completed = CountDownLatch(1)
        var result = PixelCopy.ERROR_UNKNOWN
        PixelCopy.request(surface.surface, bitmap, {
            result = it
            completed.countDown()
        }, Handler(Looper.getMainLooper()))
        assertTrue("PixelCopy must complete", completed.await(5, TimeUnit.SECONDS))
        assertEquals("PixelCopy must capture the actual native window", PixelCopy.SUCCESS, result)
        return bitmap
    }

    private fun copyCompositedSurface(): Bitmap {
        val bounds = IntArray(4)
        instrumentation.runOnMainSync {
            surfaceView.getLocationOnScreen(bounds)
            bounds[2] = surfaceView.width
            bounds[3] = surfaceView.height
        }
        val display = checkNotNull(instrumentation.uiAutomation.takeScreenshot())
        val left = bounds[0].coerceIn(0, display.width - 1)
        val top = bounds[1].coerceIn(0, display.height - 1)
        val width = bounds[2].coerceIn(1, display.width - left)
        val height = bounds[3].coerceIn(1, display.height - top)
        val region = Bitmap.createBitmap(display, left, top, width, height)
        val copy = checkNotNull(region.copy(Bitmap.Config.ARGB_8888, false))
        if (region !== display) region.recycle()
        display.recycle()
        return copy
    }

    private fun initialize(renderer: Int) = onFrame {
        NativePpsspp.nativeSetAppContext(context.applicationContext)
        NativePpsspp.nativeSetShaderPreset("")
        NativePpsspp.nativeSetRenderer(renderer)
        NativePpsspp.nativeInit(
            context.applicationInfo.sourceDir,
            File(root, "data").apply { mkdirs() }.path,
            File(root, "memstick").apply { mkdirs() }.path,
            root.path, 480, 272, 60f
        )
        NativePpsspp.nativeSetAudioDeviceInfo(48000, 192)
        InstrumentationRegistry.getArguments().getString("nativeLogLevel")?.let {
            NativePpsspp.nativeSetConfig("log_level", it)
        }
        NativePpsspp.nativeSetConfig("audio_backend", "aaudio")
        NativePpsspp.nativeSetConfig("audio_low_latency", "0")
        NativePpsspp.nativeSetConfig("audio_buffer_ms", "0")
        NativePpsspp.nativeSetConfig("ppsspp_skip_buffer_effects", "disabled")
        NativePpsspp.nativeSetConfig("fps_limit", "0")
        NativePpsspp.nativeSetPaused(false)
        val surface = checkNotNull(publishedSurface)
        assertTrue("Attach renderer=$renderer", NativePpsspp.nativeSetSurface(surface.surface, surface.width, surface.height))
    }

    private fun awaitBoot() {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(20)
        while (System.nanoTime() < deadline) {
            if (onFrame {
                    NativePpsspp.nativeRunFrame()
                    assertTrue("Boot error: ${NativePpsspp.nativeGetRuntimeError()}", NativePpsspp.nativeGetRuntimeError().isEmpty())
                    NativePpsspp.nativeIsBooted()
                }) return
            Thread.sleep(10)
        }
        throw AssertionError("The generated PSP guest did not boot in 20 seconds")
    }

    private fun assertFrameProgress() {
        val first = NativePpsspp.nativeRunFrame()
        val second = NativePpsspp.nativeRunFrame()
        assertTrue("Guest clock must advance across native frames ($first -> $second)", first >= 0 && second > first)
    }

    private fun state(): IntArray = NativeCoreDiagnostics.nativeGetRuntimeState().also {
        assertTrue("Runtime snapshot must contain all documented fields", it.size >= 9)
    }

    private fun <T> onFrame(action: () -> T): T =
        executor.submit(Callable { action() }).get(25, TimeUnit.SECONDS)

    private fun createSurface(width: Int, height: Int, keepExisting: Boolean = false) {
        instrumentation.runOnMainSync {
            if (!::surfaceContainer.isInitialized) {
                surfaceContainer = FrameLayout(activity)
                activity.setContentView(surfaceContainer)
            }
            if (!keepExisting) surfaceContainer.removeAllViews()
            publishedSurface = null
            surfaceView = SurfaceView(activity).apply {
                holder.setFormat(PixelFormat.OPAQUE)
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: SurfaceHolder) = Unit
                    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                        publishedSurface = HostSurface(holder.surface, width, height)
                    }
                    override fun surfaceDestroyed(holder: SurfaceHolder) {
                        if (publishedSurface?.surface === holder.surface) publishedSurface = null
                    }
                })
                holder.setFixedSize(width, height)
            }
            surfaceContainer.addView(surfaceView)
        }
        awaitSurface(width, height)
    }

    private fun awaitSurface(width: Int, height: Int): HostSurface {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10)
        while (System.nanoTime() < deadline) {
            publishedSurface?.let { if (it.surface.isValid && it.width == width && it.height == height) return it }
            Thread.sleep(10)
        }
        throw AssertionError("Android did not publish a valid $width x $height surface")
    }

    companion object {
        private const val BOOTED = 0
        private const val AUDIO = 1
        private const val CPU = 2
        private const val FPS_MODE = 3
        private const val TARGET_FPS = 4
        private const val SOFTWARE = 5
        private const val REPLACE_TEXTURES = 6
        private const val WIDTH = 7
        private const val HEIGHT = 8

        /** Writes a solid PSP8888 VRAM framebuffer and presents it via real HLE imports. */
        private fun displayPspElf(): ByteArray {
            val entry = 0x08804000
            val bytes = minimalPspElf().copyOf(0x6A0)
            bytes.fill(0, 0x100)
            val elf = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
            fun word(offset: Int, value: Int) { elf.putInt(offset, value) }
            fun half(offset: Int, value: Int) { elf.putShort(offset, value.toShort()) }
            fun jump(opcode: Int, address: Int) = opcode or ((address ushr 2) and 0x03FFFFFF)
            word(32, 0x600)
            word(64, 0x300) // PSP module-info file offset.
            word(68, 0x2A8)
            val code = intArrayOf(
                0x3C080400, 0x3C09FF33, 0x352966CC, 0x3C0A0002, 0x354A2000,
                0xAD090000.toInt(), 0x25080004, 0x254AFFFF, 0x1540FFFC, 0,
                0x24040000, 0x240501E0, 0x24060110, jump(0x0C000000, entry + 0x290), 0,
                0x3C040400, 0x24050200, 0x24060003, 0x24070001, jump(0x0C000000, entry + 0x298), 0,
                jump(0x0C000000, entry + 0x2A0), 0, jump(0x08000000, entry + 21 * 4), 0
            )
            code.forEachIndexed { index, instruction -> word(0x100 + index * 4, instruction) }
            half(0x300, 0); half(0x302, 0x0100)
            "NativeDisplay".toByteArray(Charsets.US_ASCII).copyInto(bytes, 0x304)
            word(0x32C, entry + 0x240); word(0x330, entry + 0x254)
            word(0x340, entry + 0x260)
            half(0x344, 0x0100); half(0x346, 0x4001); bytes[0x348] = 5; half(0x34A, 3)
            word(0x34C, entry + 0x280); word(0x350, entry + 0x290)
            "sceDisplay".toByteArray(Charsets.US_ASCII).copyInto(bytes, 0x360)
            word(0x380, 0x0E20F177) // sceDisplaySetMode.
            word(0x384, 0x289D82FE) // sceDisplaySetFrameBuf.
            word(0x388, 0x984C27E7.toInt()) // sceDisplayWaitVblankStart.
            for (stub in listOf(0x390, 0x398, 0x3A0)) { word(stub, 0x03E00008); word(stub + 4, 0) }
            val names = "\u0000.text\u0000.rodata.sceModuleInfo\u0000.shstrtab\u0000"
            names.toByteArray(Charsets.US_ASCII).copyInto(bytes, 0x500)
            fun section(index: Int, name: String, type: Int, flags: Int, address: Int, offset: Int, size: Int) {
                val header = 0x600 + index * 40
                word(header, names.indexOf(name)); word(header + 4, type); word(header + 8, flags)
                word(header + 12, address); word(header + 16, offset); word(header + 20, size); word(header + 32, 4)
            }
            section(1, ".text", 1, 6, entry, 0x100, code.size * 4)
            section(2, ".rodata.sceModuleInfo", 1, 2, entry + 0x200, 0x300, 52)
            section(3, ".shstrtab", 3, 0, 0, 0x500, names.length)
            return bytes
        }

        /** ELF32/MIPS executable: j entry; nop, plus user-mode PSP module info. */
        private fun minimalPspElf(exitImmediately: Boolean = false): ByteArray {
            val entry = 0x08804000
            val bytes = ByteArray(0x2A0)
            val elf = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
            fun word(offset: Int, value: Int) { elf.putInt(offset, value) }
            fun half(offset: Int, value: Int) { elf.putShort(offset, value.toShort()) }
            bytes[0] = 0x7F
            bytes[1] = 'E'.code.toByte(); bytes[2] = 'L'.code.toByte(); bytes[3] = 'F'.code.toByte()
            bytes[4] = 1; bytes[5] = 1; bytes[6] = 1
            half(16, 2) // ET_EXEC, already relocated.
            half(18, 8) // EM_MIPS.
            word(20, 1); word(24, entry); word(28, 52); word(32, 0x200)
            half(40, 52); half(42, 32); half(44, 1); half(46, 40); half(48, 4); half(50, 3)
            word(52, 1); word(56, 0x100); word(60, entry); word(64, 0x110)
            word(68, 0x50); word(72, 0x1000); word(76, 7); word(80, 0x100)
            word(0x100, 0x08000000 or ((entry ushr 2) and 0x03FFFFFF))
            word(0x104, 0) // Delay slot.
            half(0x110, 0); half(0x112, 0x0100)
            "NativeLifecycle".toByteArray(Charsets.US_ASCII).copyInto(bytes, 0x114)
            val names = "\u0000.text\u0000.rodata.sceModuleInfo\u0000.shstrtab\u0000"
            names.toByteArray(Charsets.US_ASCII).copyInto(bytes, 0x180)
            fun section(index: Int, name: String, type: Int, flags: Int, address: Int, offset: Int, size: Int) {
                val header = 0x200 + index * 40
                word(header, names.indexOf(name)); word(header + 4, type); word(header + 8, flags)
                word(header + 12, address); word(header + 16, offset); word(header + 20, size)
                word(header + 32, 4)
            }
            section(1, ".text", 1, 6, entry, 0x100, 8)
            section(2, ".rodata.sceModuleInfo", 1, 2, entry + 0x10, 0x110, 52)
            section(3, ".shstrtab", 3, 0, 0, 0x180, names.length)
            if (exitImmediately) {
                word(68, 0x80) // Include the import descriptor, NID and stub.
                word(0x100, 0x08000000 or (((entry + 0x78) ushr 2) and 0x03FFFFFF))
                word(0x13C, entry + 0x48) // PspModuleInfo.libstub.
                word(0x140, entry + 0x5C) // One five-word PspLibStubEntry.
                word(0x148, entry + 0x60)
                half(0x14C, 0x0100); half(0x14E, 0x4001)
                bytes[0x150] = 5; half(0x152, 1)
                word(0x154, entry + 0x70); word(0x158, entry + 0x78)
                "LoadExecForUser".toByteArray(Charsets.US_ASCII).copyInto(bytes, 0x160)
                word(0x170, 0x05572A5F) // sceKernelExitGame.
                word(0x178, 0x03E00008); word(0x17C, 0) // Import stub patched by the real loader.
            }
            return bytes
        }
    }
}

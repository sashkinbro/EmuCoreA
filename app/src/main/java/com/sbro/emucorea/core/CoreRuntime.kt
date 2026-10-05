// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreA-Proprietary
package com.sbro.emucorea.core

import android.content.Context
import android.graphics.Rect
import android.net.Uri
import android.os.Build
import android.os.ParcelFileDescriptor
import android.os.Process
import android.util.Log
import android.view.Surface
import android.view.WindowManager
import java.io.File
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicIntegerArray
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.thread
import kotlin.concurrent.withLock
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import com.sbro.emucorea.data.RetroArchShaderEffects

/**
 * Process-wide owner of the native PPSSPP session.
 *
 * The core ([NativePpsspp], libemucorea_core.so) is a process singleton;
 * [sessionLock] serialises every call that touches it, including surface
 * attach/detach and the frame worker. The native core owns audio and
 * presentation, so Kotlin only drives frames, input, state and configuration.
 */
internal object CoreRuntime {
    private const val TAG = "CoreRuntime"
    private const val DEFAULT_FRAME_WIDTH = 480
    private const val DEFAULT_FRAME_HEIGHT = 272
    private const val SAVE_STATE_MAGIC = 0x54534345
    private const val SAVE_STATE_VERSION = 1
    private const val SAVE_STATE_HEADER_BYTES = 8
    private const val DEFAULT_SAVE_STATE_BUFFER_BYTES = 256 * 1024
    // Holding rewind steps back through history at a steady pace.
    private const val REWIND_STEP_INTERVAL_NANOS = 500_000_000L

    val settings = ConcurrentHashMap<String, String>()
    private val _failure = MutableStateFlow<RuntimeFailure?>(null)
    val failure = _failure.asStateFlow()

    private val lifecycleLock = ReentrantLock()
    private val sessionLock = ReentrantLock()
    private var context: Context? = null
    private var nativeDataDirectory = ""
    private var externalDataDirectory = ""
    @Volatile private var nativeInitialized = false
    @Volatile private var nativeSurfaceReady = false
    @Volatile private var sessionActive = false
    @Volatile private var booted = false
    @Volatile private var pendingGamePath: String? = null
    // Snapshot restored once the re-booted core reports ready (renderer switch).
    @Volatile private var pendingStateRestorePath: String? = null
    @Volatile private var pendingStateRestoreAttempts = 0
    // SAF descriptor for a content:// game. Kept open for the whole session;
    // the native core duplicates the fd and owns only its own duplicate.
    private var pendingGameDescriptor: ParcelFileDescriptor? = null
    @Volatile private var worker: Thread? = null

    private var systemDirectory = ""
    private var saveDirectory = ""
    private var coreAssetsDirectory = ""

    @Volatile private var running = false
    @Volatile private var paused = false
    @Volatile private var timeControlMode = 0
    @Volatile private var surface: Surface? = null
    @Volatile private var surfaceWidth = 0
    @Volatile private var surfaceHeight = 0
    @Volatile private var renderedFirstFrame = false
    @Volatile private var sessionStartedAtNanos = 0L
    @Volatile private var frameWidth = DEFAULT_FRAME_WIDTH
    @Volatile private var frameHeight = DEFAULT_FRAME_HEIGHT
    @Volatile private var requestedRenderer = RendererDefaults.defaultForHardware()
    @Volatile private var currentGamePath: String? = null
    @Volatile private var currentBiosOnly = false
    @Volatile private var performanceMetricsEnabled = false
    @Volatile private var detailedPerformanceMetrics = false
    @Volatile private var performanceMetricsSnapshot: String? = null

    private val desiredPadButtons = AtomicIntegerArray(IntArray(2) { 0xFFFF })
    private val pendingPadPressEdges = AtomicIntegerArray(2)
    // A short physical or touch tap can finish before PPSSPP polls input.
    // Keep each edge visible for three frontend frames.
    private val padEdgeHoldMask = IntArray(2)
    private val padEdgeHoldFrames = IntArray(2)
    private val pendingPadAnalog = AtomicIntegerArray(IntArray(2) { 0x80808080.toInt() })

    private class FrameTask(
        val block: () -> Boolean,
        val completed: CountDownLatch
    ) {
        @Volatile var result: Boolean = false
    }

    private val frameTasks = ConcurrentLinkedQueue<FrameTask>()

    /**
     * Runs [block] on the frame worker thread.
     *
     * Save states and configuration that must not race a native frame are
     * serialised there; calls made from the worker itself run inline.
     */
    private fun runOnFrameThread(block: () -> Boolean): Boolean {
        val activeWorker = worker
        if (activeWorker == null || !activeWorker.isAlive || activeWorker === Thread.currentThread()) {
            return block()
        }
        val task = FrameTask(block, CountDownLatch(1))
        frameTasks.add(task)
        return try {
            if (!task.completed.await(30, TimeUnit.SECONDS)) {
                Log.w(TAG, "Timed out waiting for the frame worker to run a state operation")
                false
            } else {
                task.result
            }
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
            false
        }
    }

    private fun drainFrameTasks() {
        while (true) {
            val task = frameTasks.poll() ?: return
            task.result = try {
                task.block()
            } catch (error: Throwable) {
                Log.e(TAG, "Frame-thread state operation failed", error)
                false
            }
            task.completed.countDown()
        }
    }

    fun initialize(context: Context, externalDataRoot: String? = null) {
        this.context = context.applicationContext
        val root = File(context.filesDir, "ppsspp")
        systemDirectory = File(root, "system").apply { mkdirs() }.absolutePath
        saveDirectory = File(root, "save").apply { mkdirs() }.absolutePath
        coreAssetsDirectory = File(root, "assets").apply { mkdirs() }.absolutePath
        nativeDataDirectory = File(root, "native").apply { mkdirs() }.absolutePath
        // The core's memStickDirectory must match the app's emulator data root,
        // otherwise core-owned data (SAVEDATA, TEXTURES, CHEATS, NAND) splits
        // off into Android/data while the app reads the chosen folder.
        externalDataDirectory = externalDataRoot?.takeIf { it.isNotBlank() }
            ?: (context.getExternalFilesDir(null) ?: context.filesDir).absolutePath
        SwanStationOptions.initialize(context.applicationContext)
        runCatching {
            extractCoreAssets(context.applicationContext)
            ensureNativeCoreLocked()
        }.onFailure { Log.e(TAG, "Unable to initialise the native PPSSPP core", it) }
    }

    /**
     * PPSSPP reads its fonts, shaders and compatibility data from
     * <systemDirectory>/PPSSPP. The files ship inside the APK assets, so copy
     * them out once per app version where the core can open them as files.
     */
    private fun extractCoreAssets(context: Context) {
        val target = File(systemDirectory, "PPSSPP")
        @Suppress("DEPRECATION")
        val version = runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionCode
        }.getOrDefault(0)
        val stamp = File(target, ".asset-version")
        if (stamp.isFile && stamp.readText() == version.toString()) return
        target.mkdirs()
        copyAssetTree(context, "", target)
        stamp.writeText(version.toString())
    }

    private fun copyAssetTree(context: Context, assetPath: String, target: File) {
        val children = context.assets.list(assetPath).orEmpty()
        if (children.isEmpty()) {
            target.parentFile?.mkdirs()
            context.assets.open(assetPath).use { input ->
                target.outputStream().use { output -> input.copyTo(output) }
            }
            return
        }
        target.mkdirs()
        for (child in children) {
            if (assetPath.isEmpty() && child == "catalog") continue
            val childPath = if (assetPath.isEmpty()) child else "$assetPath/$child"
            copyAssetTree(context, childPath, File(target, child))
        }
    }

    /**
     * Loads libemucorea_core and creates its process-wide state. The native
     * frontend is shut down with [NativePpsspp.nativeShutdown] between
     * sessions, so this can run again for the next game.
     */
    private fun ensureNativeCoreLocked(): Boolean {
        if (nativeInitialized) return true
        val appContext = context ?: return false
        if (!NativePpsspp.ensureLoaded()) {
            Log.e(TAG, "libemucorea_core is unavailable")
            return false
        }
        // PPSSPP's PPGe text renderer needs the application context before the
        // first PPGe draw (savedata dialogs would otherwise crash under
        // CheckJNI with an invalid class reference).
        runCatching { NativePpsspp.nativeSetAppContext(appContext) }
            .onFailure { Log.w(TAG, "Unable to set the native app context", it) }
        // The context is created inside nativeInit, so the renderer must be
        // pushed first. This is a no-op for the Vulkan default.
        runCatching {
            NativePpsspp.nativeSetRenderer(RendererDefaults.toCoreRenderer(requestedRenderer))
        }.onFailure { Log.w(TAG, "Unable to select the renderer", it) }
        val metrics = appContext.resources.displayMetrics
        val externalDir = externalDataDirectory.takeIf { it.isNotBlank() }
            ?: (appContext.getExternalFilesDir(null) ?: appContext.filesDir).absolutePath
        NativePpsspp.nativeInit(
            appContext.packageCodePath,
            nativeDataDirectory,
            externalDir,
            appContext.cacheDir.absolutePath,
            metrics.widthPixels,
            metrics.heightPixels,
            displayRefreshRate(appContext)
        )
        nativeInitialized = true
        return true
    }

    /**
     * The app's emulator data root changed. The core derives SAVEDATA,
     * TEXTURES, CHEATS and NAND from it, so repoint the live core (when idle)
     * and remember it for the next nativeInit.
     */
    fun setExternalDataDirectory(path: String) {
        if (path.isBlank()) return
        externalDataDirectory = path
        if (!nativeInitialized) return
        runCatching {
            NativePpsspp.nativeUpdateDataDirectories(nativeDataDirectory, externalDataDirectory)
        }.onFailure { Log.w(TAG, "Unable to update the native data directories", it) }
    }

    private fun displayRefreshRate(context: Context): Float = runCatching {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            context.display.refreshRate
        } else {
            @Suppress("DEPRECATION")
            (context.getSystemService(Context.WINDOW_SERVICE) as WindowManager)
                .defaultDisplay.refreshRate
        }
    }.getOrDefault(60.0f)

    fun isRunning(): Boolean = running && failure.value == null
    fun hasSession(): Boolean = running || booted

    fun setPerformanceMetricsEnabled(visible: Boolean, detailed: Boolean) {
        performanceMetricsEnabled = visible
        detailedPerformanceMetrics = visible && detailed
        if (!visible) performanceMetricsSnapshot = null
    }

    fun performanceMetricsSnapshot(): String? = performanceMetricsSnapshot

    fun setAudioGain(volume: Int, muted: Boolean) {
        val percent = if (muted) 0 else volume.coerceIn(AudioDefaults.VOLUME_MIN, AudioDefaults.VOLUME_MAX)
        NativePpsspp.nativeSetConfig("volume", percent.toString())
    }

    fun start(gamePath: String, biosOnly: Boolean): Boolean = lifecycleLock.withLock {
        startSession(gamePath, biosOnly)
    }

    /**
     * Switches the renderer. The native graphics context is re-negotiated on
     * boot, so a live session is restarted in place: the running game is
     * snapshotted, the session torn down, the new renderer applied, the game
     * re-booted and the snapshot restored as soon as the core finishes booting.
     */
    fun restartWithRenderer(renderer: Int): Boolean = lifecycleLock.withLock {
        requestedRenderer = RendererDefaults.normalizeAndroidRenderer(renderer)
        val coreRenderer = RendererDefaults.toCoreRenderer(requestedRenderer)
        runCatching { NativePpsspp.nativeSetRenderer(coreRenderer) }
            .onFailure { Log.w(TAG, "Unable to store the renderer", it) }
        Log.i(TAG, "Renderer preference set: ${RendererDefaults.coreRendererName(coreRenderer)}")

        val gamePath = currentGamePath
        val hasLiveSession = (sessionActive || running) && !gamePath.isNullOrBlank()
        if (!hasLiveSession) {
            // Idle: the native side rebuilds its context immediately.
            return@withLock true
        }

        val appContext = context
        val snapshot: File? = if (appContext != null) {
            File(appContext.cacheDir, ".renderer-switch.sav").also { it.delete() }
        } else {
            null
        }
        val savedState = snapshot != null && saveState(snapshot.absolutePath)
        shutdownSession()
        if (!startSession(gamePath, false)) {
            Log.e(TAG, "Renderer restart failed; the game could not be re-booted")
            if (snapshot != null) snapshot.delete()
            reportFailure("The renderer restart failed")
            return@withLock false
        }
        if (savedState && snapshot != null) {
            // The core needs its first frame before a state can be loaded, so
            // the frame loop picks this up and retries until it takes.
            pendingStateRestoreAttempts = 0
            pendingStateRestorePath = snapshot.absolutePath
        } else if (snapshot != null) {
            snapshot.delete()
        }
        Log.i(TAG, "Renderer restarted with ${RendererDefaults.coreRendererName(coreRenderer)}")
        true
    }

    private fun startSession(gamePath: String, biosOnly: Boolean): Boolean {
        val startupStartedAtNanos = System.nanoTime()
        if (biosOnly || gamePath.isBlank()) {
            // PPSSPP cannot identify content from an empty path, so a BIOS-only
            // session is not bootable. Report it instead of crashing.
            Log.w(TAG, "BIOS-only boot is not supported: nativeBoot requires a game image")
            return false
        }
        if (!isSupportedDiscPath(gamePath)) {
            Log.e(TAG, "Unsupported PSP image: $gamePath")
            return false
        }
        // A live session must be fully torn down (PSP_Shutdown + graphics) before
        // the core can load another image; the initial one just reuses the core
        // created by initialize().
        if (sessionActive || running) shutdownSession()
        if (!ensureNativeCoreLocked()) {
            Log.e(TAG, "Native PPSSPP core is not initialised")
            return false
        }
        val prepared = prepareLaunchPath(gamePath)
        if (prepared == null) {
            Log.e(TAG, "Unable to prepare PSP image: $gamePath")
            shutdownSession()
            return false
        }
        currentGamePath = gamePath
        currentBiosOnly = false
        pendingGamePath = prepared
        applyStartOptions()
        for (port in 0..1) {
            desiredPadButtons.set(port, 0xFFFF)
            pendingPadPressEdges.set(port, 0)
            padEdgeHoldMask[port] = 0
            padEdgeHoldFrames[port] = 0
        }
        running = true
        sessionActive = true
        paused = false
        renderedFirstFrame = false
        sessionStartedAtNanos = startupStartedAtNanos
        var started = false
        try {
            worker = thread(name = "EmuCoreA-Frame", isDaemon = true, start = true) {
                Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY)
                runLoop()
            }
            // The core can only boot once a surface exists, so a session that
            // starts before the SurfaceView is ready waits for attachSurface().
            val currentSurface = surface
            if (currentSurface != null) {
                attachSurface(currentSurface, surfaceWidth, surfaceHeight)
            }
            Log.i(TAG, String.format(Locale.US, "Startup setup %.1f ms",
                (System.nanoTime() - startupStartedAtNanos) / 1_000_000.0))
            started = true
            return true
        } catch (error: Exception) {
            Log.e(TAG, "Failed to start the native frame runtime", error)
            return false
        } finally {
            if (!started) shutdownSession()
        }
    }

    /** Publishes the frontend filters and core options the app owns before boot. */
    private fun applyStartOptions() {
        // The renderer must be known before the core creates its graphics
        // context; nativeSetRenderer rebuilds an idle context if needed.
        runCatching {
            NativePpsspp.nativeSetRenderer(RendererDefaults.toCoreRenderer(requestedRenderer))
        }.onFailure { Log.w(TAG, "Unable to apply the renderer", it) }
        val upscale = settings["EmuCoreA/Display:Upscale"]?.toFloatOrNull()
            ?: settings["EmuCoreA:UpscaleMultiplier"]?.toFloatOrNull()
        upscale?.let(::pushInternalResolution)
        settings["EmuCoreA:EnableFastMem"]?.toBooleanStrictOrNull()?.let { fastMemory ->
            forwardCoreOption("ppsspp_fast_memory", if (fastMemory) "enabled" else "disabled")
        }
        settings["EmuCoreA:EnableEERecompiler"]?.toBooleanStrictOrNull()?.let { jit ->
            // The PSP has a single CPU recompiler; the app's PS1-era toggle maps
            // to the closest CPU core choice.
            forwardCoreOption("ppsspp_cpu_core", if (jit) "JIT" else "IR JIT")
        }
        forwardCoreOption("ppsspp_texture_filtering", pspTextureFilterName())
        pushShaderEffect()
        pushShaderPreset()
        settings["EmuCoreA/GS:RewindEnabled"]?.toBooleanStrictOrNull()?.let {
            NativePpsspp.nativeSetRewindEnabled(it)
        }
        // Every core option is pushed with its effective value (user override
        // or catalogue default) so the native defaults can never drift from
        // what the UI shows. Options the app manages itself (backend/software
        // rendering/internal resolution) are filtered by translateCoreOption.
        PpssppCoreOptions.all().forEach { option ->
            val stored = SwanStationOptions.value(option.key)
            val value = stored ?: when (option.key) {
                // The legacy app-level GS filter still derives the default.
                "ppsspp_texture_filtering" -> pspTextureFilterName()
                else -> option.defaultValue
            }
            forwardCoreOption(option.key, value)
        }
        // Full catalogue overrides win over the derived defaults as well.
        SwanStationOptions.persistedEntries().forEach { (key, value) ->
            forwardCoreOption(key, value)
        }
        // Internal resolution is owned by the app's per-game upscale setting, so
        // re-assert it after the persisted store so a stale entry cannot shadow it.
        upscale?.let(::pushInternalResolution)
        // Aspect ratio is also owned by the app (the core has no option for it).
        settings["EmuCoreA/GS:AspectRatio"]?.let {
            NativePpsspp.nativeSetConfig("aspect_ratio", it)
        }
        settings["EmuCoreA/Display:AspectRatio"]?.toIntOrNull()?.let {
            NativePpsspp.nativeSetConfig("aspect_ratio", aspectRatioValue(it))
        }
    }

    private fun pushInternalResolution(preference: Float) {
        val scale = Math.round(preference).coerceIn(1, 10)
        NativePpsspp.nativeSetConfig("internal_resolution", scale.toString())
    }

    /**
     * Pushes the app's "Frame limit / target FPS" settings into the core.
     * 0 means the normal region rate; a positive target limits the frame rate.
     */
    private fun pushFpsLimit() {
        val enabled = settings["EmuCoreA/GS:FrameLimitEnable"]?.toBooleanStrictOrNull() ?: false
        val target = settings["EmuCoreA/GS:TargetFps"]?.toIntOrNull() ?: 0
        val limit = if (enabled && target > 0) target.coerceIn(20, 120) else 0
        NativePpsspp.nativeSetConfig("fps_limit", limit.toString())
    }

    private fun currentShaderEffect(): Int {
        val enabled = settings["EmuCoreA/GS:ShaderChainEnabled"]?.toBooleanStrictOrNull() == true
        if (!enabled) return RetroArchShaderEffects.NONE
        return RetroArchShaderEffects.classify(settings["EmuCoreA/GS:ShaderChainPreset"])
    }

    /**
     * Stores the frontend's shader selection in the native core. The values are
     * not rendered yet; a future presentation hook consumes them.
     */
    private fun pushShaderEffect() {
        runCatching { NativePpsspp.nativeSetShaderEffect(currentShaderEffect()) }
            .onFailure { Log.w(TAG, "Unable to apply shader effect", it) }
    }

    private fun pushShaderPreset() {
        val enabled = settings["EmuCoreA/GS:ShaderChainEnabled"]?.toBooleanStrictOrNull() == true
        val preset = settings["EmuCoreA/GS:ShaderChainPreset"].orEmpty()
        runCatching { NativePpsspp.nativeSetShaderPreset(if (enabled) preset else "") }
            .onFailure { Log.w(TAG, "Unable to apply shader preset", it) }
    }

    private fun pspTextureFilterName(): String {
        // The shared GS filter setting defaults to 0, which the PS1 mapping reads
        // as Nearest. PPSSPP's own default is Auto, and forcing nearest turns
        // low-resolution effects such as Tekken 6's title flame into blocks, so 0
        // means Auto here. An explicit choice in the PSP core options ("Texture
        // Filtering") is applied afterwards and still wins.
        val filter = settings["EmuCoreA/GS:filter"]?.toIntOrNull() ?: return "Auto"
        return when {
            filter <= 0 -> "Auto"
            filter == 1 -> "Linear"
            else -> "Auto max quality"
        }
    }

    /**
     * Translates a PPSSPP libretro option key into the native core config
     * contract. Unknown keys are ignored silently: the settings store still
     * carries the legacy catalogue, but only the options the native frontend
     * understands reach the core.
     */
    private fun forwardCoreOption(key: String, value: String) {
        val config = translateCoreOption(key, value) ?: return
        NativePpsspp.nativeSetConfig(config.first, config.second)
    }

    internal fun translateCoreOption(key: String, value: String): Pair<String, String>? {
        // The native core understands the full libretro option catalogue and
        // parses the same value strings the UI uses, so every ppsspp_* key is
        // forwarded verbatim. The two exceptions are owned by the app's own
        // renderer controls and would otherwise fight nativeSetRenderer.
        if (key.startsWith("ppsspp_")) {
            return when (key) {
                "ppsspp_backend", "ppsspp_software_rendering" -> null
                else -> key to value
            }
        }
        // Native short keys may also be pushed directly.
        return when (key) {
            "internal_resolution", "frameskip", "auto_frameskip", "frame_duplication",
            "texture_filtering", "texture_scaling_level", "volume", "skip_buffer_effects",
            "fast_memory", "cpu_core", "crop16x9", "vsync", "multi_threading" -> key to value
            else -> null
        }
    }

    private fun nativeBoolValue(value: String): String? = when (value.trim().lowercase(Locale.US)) {
        "enabled", "true", "1", "on", "yes" -> "1"
        "disabled", "false", "0", "off", "no" -> "0"
        else -> null
    }

    private fun parseInternalResolution(value: String): String? {
        val trimmed = value.trim().lowercase(Locale.US)
        if (trimmed.endsWith("x")) {
            return trimmed.removeSuffix("x").toIntOrNull()?.coerceIn(1, 10)?.toString()
        }
        val width = trimmed.substringBefore('x').toIntOrNull() ?: return null
        return ((width + 240) / 480).coerceIn(1, 10).toString()
    }

    /** Persists and forwards a PPSSPP core option. */
    fun setCoreOption(key: String, value: String) {
        SwanStationOptions.set(key, value)
        forwardCoreOption(key, value)
    }

    fun setTimeControl(mode: Int) {
        val normalized = mode.coerceIn(0, 2)
        val previous = timeControlMode
        if (normalized == previous) return
        timeControlMode = normalized
        when {
            normalized == 1 -> NativePpsspp.nativeSetConfig("fast_forward", "1")
            previous == 1 -> NativePpsspp.nativeSetConfig("fast_forward", "0")
        }
        if (normalized == 2) {
            // Hold-to-rewind must work even when the option was never toggled:
            // start capturing snapshots right away (the first snapshot takes
            // two seconds, then the control rewinds).
            runCatching { NativePpsspp.nativeSetRewindEnabled(true) }
        }
        if (previous == 2 && normalized != 2) {
            // Rewinding pauses the core in stepping mode; release resumes it.
            runCatching { NativePpsspp.nativeRewindRelease() }
        }
    }

    @Volatile private var audioOutputLatencyMs = AudioDefaults.OUTPUT_LATENCY_MS_DEFAULT
    @Volatile private var audioLowLatency = false
    // Rebuilding the OpenSL player is expensive, so dragging the latency slider
    // coalesces into a single apply shortly after the user stops.
    private val audioBufferScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var audioBufferApplyJob: Job? = null

    /** OpenSL buffer size target in milliseconds. */
    fun setAudioBufferMs(milliseconds: Int) {
        audioOutputLatencyMs = AudioDefaults.coerceOutputLatencyMs(milliseconds)
        audioBufferApplyJob?.cancel()
        audioBufferApplyJob = audioBufferScope.launch {
            delay(400)
            pushAudioBuffer()
        }
    }

    /**
     * "Minimal output latency" trades buffer size for responsiveness: when on,
     * the OpenSL callback uses a small buffer so input lag is lower.
     */
    fun setAudioLowLatency(enabled: Boolean) {
        audioLowLatency = enabled
        audioBufferApplyJob?.cancel()
        pushAudioBuffer()
    }

    private fun pushAudioBuffer() {
        val milliseconds = if (audioLowLatency) 20 else audioOutputLatencyMs
        NativePpsspp.nativeSetConfig("audio_buffer_ms", milliseconds.toString())
    }

    /** Effective value of a core option (user override or core default). */
    fun coreOptionValue(key: String): String? =
        SwanStationOptions.value(key) ?: PpssppCoreOptions.option(key)?.defaultValue

    /**
     * Forwards a core option without persisting it. Used for per-game overrides
     * that must not pollute the global option store.
     */
    fun applyCoreOption(key: String, value: String) {
        forwardCoreOption(key, value)
    }

    /** Persists the app's aspect-ratio selection (0..4) and applies it. */
    fun setDisplayAspectRatio(type: Int) {
        val normalized = if (type in ASPECT_RATIO_STRETCH..ASPECT_RATIO_CUSTOM) type else ASPECT_RATIO_AUTO
        settings["EmuCoreA/Display:AspectRatio"] = normalized.toString()
        runCatching { NativePpsspp.nativeSetConfig("aspect_ratio", aspectRatioValue(normalized)) }
            .onFailure { Log.w(TAG, "Unable to apply the aspect ratio", it) }
    }

    private fun aspectRatioValue(type: Int): String = when (type) {
        ASPECT_RATIO_STRETCH -> "Stretch"
        2 -> "4:3"
        3 -> "16:9"
        ASPECT_RATIO_CUSTOM -> "10:7"
        else -> "Auto 4:3/3:2"
    }

    fun pause() = lifecycleLock.withLock {
        paused = true
    }

    fun resume() = lifecycleLock.withLock {
        if (!running) return@withLock
        paused = false
    }

    fun shutdown() = lifecycleLock.withLock {
        shutdownSession()
    }

    private fun shutdownSession() {
        val activeWorker = worker
        check(activeWorker !== Thread.currentThread()) {
            "The frame worker cannot synchronously shut itself down"
        }
        running = false
        timeControlMode = 0
        for (port in 0..1) {
            desiredPadButtons.set(port, 0xFFFF)
            pendingPadPressEdges.set(port, 0)
            padEdgeHoldMask[port] = 0
            padEdgeHoldFrames[port] = 0
        }
        activeWorker?.interrupt()
        var callerInterrupted = false
        try {
            if (activeWorker != null) {
                while (activeWorker.isAlive) {
                    try {
                        activeWorker.join()
                    } catch (_: InterruptedException) {
                        callerInterrupted = true
                    }
                }
            }
            worker = null
            sessionLock.withLock {
                if (nativeInitialized) {
                    NativePpsspp.nativeShutdown()
                    nativeInitialized = false
                }
                nativeSurfaceReady = false
                sessionActive = false
                booted = false
            }
            // Safe to close now: the native core only ever reads through its own
            // duplicate, and nativeShutdown has already released that one.
            closeGameDescriptor()
            pendingGamePath = null
            paused = false
            renderedFirstFrame = false
            sessionStartedAtNanos = 0L
            performanceMetricsSnapshot = null
            pendingStateRestorePath?.let {
                File(it).delete()
                File("$it.core").delete()
            }
            pendingStateRestorePath = null
            _failure.value = null
        } finally {
            if (callerInterrupted) Thread.currentThread().interrupt()
        }
    }

    fun changeDisc(path: String): Boolean {
        if (!isSupportedDiscPath(path)) {
            Log.w(TAG, "Disc swap rejected: unsupported PSP image")
            return false
        }
        Log.w(TAG, "Disc swapping is not supported by the native core")
        return false
    }

    fun saveState(path: String): Boolean {
        if (!waitForCoreBoot(20_000L)) {
            Log.e(TAG, "State save aborted: the core did not finish booting")
            return false
        }
        val target = File(path)
        target.parentFile?.mkdirs()
        val temporary = File(target.parentFile, ".${target.name}.saving")
        return try {
            val saved = runOnFrameThread {
                sessionLock.withLock {
                    booted && NativePpsspp.nativeSaveState(temporary.absolutePath)
                }
            }
            saved && writeSaveStateFile(temporary, target)
        } finally {
            temporary.delete()
        }
    }

    /** True once the native core finished its asynchronous boot. */
    fun isCoreBooted(): Boolean =
        runCatching { NativePpsspp.nativeIsBooted() }.getOrDefault(false)

    /**
     * The native boot is asynchronous and takes a couple of seconds. A state
     * can only be loaded after it completes, so wait for it (the frame loop
     * keeps running during the wait) instead of failing instantly.
     */
    private fun waitForCoreBoot(timeoutMs: Long): Boolean {
        val deadline = System.nanoTime() + timeoutMs * 1_000_000L
        while (System.nanoTime() < deadline) {
            if (isCoreBooted()) return true
            if (!running && !booted) return false
            Thread.sleep(50)
        }
        return isCoreBooted()
    }

    fun loadState(path: String): Boolean = lifecycleLock.withLock lifecycle@{
        val file = File(path)
        if (!file.isFile) return@lifecycle false
        if (!waitForCoreBoot(20_000L)) {
            Log.e(TAG, "State load aborted: the core did not finish booting")
            return@lifecycle false
        }
        val wasPaused = paused
        paused = true
        try {
            val raw = File(file.parentFile, ".${file.name}.loading")
            val prepared = extractCorePayload(file, raw)
            val loaded = prepared && runOnFrameThread {
                sessionLock.withLock {
                    booted && NativePpsspp.nativeLoadState(raw.absolutePath)
                }
            }
            raw.delete()
            if (loaded) {
                renderedFirstFrame = false
            }
            loaded
        } finally {
            paused = wasPaused
        }
    }

    /**
     * Copies the core payload out of an EmuCoreA state file into [destination]:
     * the app container is 8 bytes (magic + version) followed by the raw core
     * stream; anything without that header is copied verbatim. Streaming keeps
     * the ~35 MB states off the Java heap.
     */
    internal fun extractCorePayload(source: File, destination: File): Boolean = runCatching {
        source.inputStream().use { input ->
            val header = ByteArray(SAVE_STATE_HEADER_BYTES)
            var headerRead = 0
            while (headerRead < header.size) {
                val n = input.read(header, headerRead, header.size - headerRead)
                if (n <= 0) break
                headerRead += n
            }
            val isContainer = headerRead == header.size && readSaveStateMagic(header) == SAVE_STATE_MAGIC
            destination.outputStream().use { output ->
                if (!isContainer && headerRead > 0) output.write(header, 0, headerRead)
                input.copyTo(output, DEFAULT_SAVE_STATE_BUFFER_BYTES)
            }
        }
        true
    }.getOrElse {
        Log.e(TAG, "Unable to extract the core payload from ${source.name}", it)
        false
    }

    /** Writes the EmuCoreA state container (magic + version + core payload). */
    private fun writeSaveStateFile(rawFile: File, target: File): Boolean {
        val payload = runCatching { rawFile.readBytes() }.getOrNull() ?: return false
        val staging = File(target.parentFile, ".${target.name}.tmp")
        return try {
            staging.outputStream().use { output ->
                val header = ByteArray(SAVE_STATE_HEADER_BYTES)
                for (index in 0 until 4) {
                    header[index] = (SAVE_STATE_MAGIC ushr (index * 8)).toByte()
                    header[4 + index] = (SAVE_STATE_VERSION ushr (index * 8)).toByte()
                }
                output.write(header)
                output.write(payload)
            }
            if (staging.renameTo(target)) {
                true
            } else {
                staging.delete()
                false
            }
        } catch (_: Exception) {
            staging.delete()
            false
        }
    }

    private fun readSaveStateMagic(bytes: ByteArray): Int {
        var value = 0
        for (index in 0 until 4) value = value or ((bytes[index].toInt() and 0xFF) shl (index * 8))
        return value
    }

    @Volatile private var lastCheatFilePath: String? = null

    /** Hands the selected CWCheat file to the core, which reloads its engine. */
    fun loadCheats(path: String) {
        lastCheatFilePath = path
        applyCheats(path)
    }

    fun clearCheats() {
        lastCheatFilePath = null
        applyCheats("")
    }

    fun reloadCheats() {
        val path = lastCheatFilePath
        if (path != null) applyCheats(path) else applyCheats("")
    }

    private fun applyCheats(path: String) {
        runOnFrameThread {
            sessionLock.withLock {
                if (nativeInitialized) NativePpsspp.nativeSetCheats(path)
            }
            true
        }
    }

    fun setMemoryCardPath(slot: Int, path: String?) {
        // No-op: the native core owns its memory card layout.
    }

    fun setTextureReplacementsPathOverride(path: String?) {
        // No-op: texture replacements are configured through PPSSPP settings.
    }

    fun setPadButtons(port: Int, buttons: Int): Boolean {
        if (port !in 0..1) return false
        val next = buttons and 0xFFFF
        val previous = desiredPadButtons.getAndSet(port, next)
        val pressedEdges = previous and next.inv() and 0xFFFF
        if (pressedEdges != 0) {
            while (true) {
                val queued = pendingPadPressEdges.get(port)
                if (pendingPadPressEdges.compareAndSet(port, queued, queued or pressedEdges)) break
            }
        }
        return true
    }

    fun setPadAnalog(port: Int, lx: Int, ly: Int, rx: Int, ry: Int): Boolean {
        if (port !in 0..1) return false
        val packed = lx.coerceIn(0, 255) or
            (ly.coerceIn(0, 255) shl 8) or
            (rx.coerceIn(0, 255) shl 16) or
            (ry.coerceIn(0, 255) shl 24)
        pendingPadAnalog.set(port, packed)
        return true
    }

    fun setPadAnalogMode(port: Int, enabled: Boolean): Boolean {
        // The native core always reports analog-capable pads.
        return port in 0..1
    }

    fun togglePadAnalogMode(port: Int): Boolean? {
        // Analog mode is implicit on the native path; no core state to query.
        return null
    }

    fun getPadRumble(port: Int): FloatArray? = null

    fun attachSurface(value: Surface, width: Int, height: Int) {
        val previous = surface
        surface = value
        surfaceWidth = width
        surfaceHeight = height
        sessionLock.withLock {
            if (!(nativeSurfaceReady && previous === value)) {
                if (nativeSurfaceReady) {
                    NativePpsspp.nativeSetSurface(null, 0, 0)
                    nativeSurfaceReady = false
                }
                if (nativeInitialized) {
                    nativeSurfaceReady = NativePpsspp.nativeSetSurface(value, width, height)
                    if (!nativeSurfaceReady) {
                        Log.e(TAG, "Failed to attach the Vulkan presentation surface")
                    }
                }
            }
            bootPendingIfReadyLocked()
        }
    }

    private fun bootPendingIfReadyLocked() {
        if (booted || !nativeSurfaceReady || !nativeInitialized) return
        val path = pendingGamePath ?: return
        if (path.startsWith("content://")) {
            val descriptor = openGameDescriptor(path)
            val started = descriptor != null &&
                NativePpsspp.nativeBootFd(descriptor.fd, safPathHint(path))
            if (!started) closeGameDescriptor()
            finishBoot(path, started)
        } else {
            finishBoot(path, NativePpsspp.nativeBoot(path))
        }
    }

    private fun finishBoot(path: String, started: Boolean) {
        if (started) {
            booted = true
            Log.i(TAG, "Native game boot started")
        } else {
            Log.e(TAG, "nativeBoot failed for $path")
            reportFailure("The native core could not boot $path")
        }
    }

    /**
     * Opens the document descriptor for a content:// game. The descriptor is
     * kept open until the session ends; the native loader duplicates the fd, so
     * closing it here only releases the frontend's own reference.
     */
    private fun openGameDescriptor(path: String): ParcelFileDescriptor? {
        closeGameDescriptor()
        val app = context ?: return null
        return runCatching {
            app.contentResolver.openFileDescriptor(Uri.parse(path), "r")
        }.onFailure {
            Log.e(TAG, "Unable to open SAF descriptor for $path", it)
        }.getOrNull()?.also { pendingGameDescriptor = it }
    }

    private fun closeGameDescriptor() {
        val descriptor = pendingGameDescriptor ?: return
        pendingGameDescriptor = null
        runCatching { descriptor.close() }
    }

    /** File name hint for the native loader, so Identify_File sees the extension. */
    private fun safPathHint(path: String): String {
        val app = context ?: return DocumentPathResolver.getFallbackDisplayName(path)
        return runCatching { DocumentPathResolver.getDisplayName(app, path) }
            .getOrNull()
            ?.takeIf { it.isNotBlank() }
            ?: DocumentPathResolver.getFallbackDisplayName(path)
    }

    fun hasAttachedSurface(value: Surface, width: Int, height: Int): Boolean =
        sessionLock.withLock {
            nativeSurfaceReady && surface === value && surfaceWidth == width && surfaceHeight == height
        }

    fun detachSurface() {
        sessionLock.withLock {
            if (nativeSurfaceReady) {
                NativePpsspp.nativeSetSurface(null, 0, 0)
                nativeSurfaceReady = false
            }
        }
        surface = null
        surfaceWidth = 0
        surfaceHeight = 0
        renderedFirstFrame = false
    }

    fun displayRect(): FloatArray? {
        if (!renderedFirstFrame || surfaceWidth <= 0 || surfaceHeight <= 0) return null
        // The native core letterboxes using PPSSPP's display layout; the raw
        // frame pixel size is the best frontend-side approximation of it.
        val rect = fitRect(surfaceWidth, surfaceHeight, frameWidth, frameHeight)
        return floatArrayOf(rect.left.toFloat(), rect.top.toFloat(), rect.right.toFloat(), rect.bottom.toFloat())
    }

    fun diagnostics(): String =
        "native core: loaded=${if (nativeInitialized) 1 else 0} booted=${if (booted) 1 else 0} " +
            "renderer=${RendererDefaults.coreRendererName(activeCoreRenderer())}"

    fun gpuBackendSubmissions(): Long = 0L

    fun updateSetting(section: String, key: String, value: String): Boolean {
        if ((section == "EmuCoreA" || section == "EmuCoreA/GS") && key == "Renderer") {
            val renderer = value.toIntOrNull() ?: return false
            requestedRenderer = RendererDefaults.normalizeAndroidRenderer(renderer)
            settings["$section:$key"] = value
            return true
        }
        if ((section == "EmuCoreA" || section == "EmuCoreA/GS") && key == "RewindEnabled") {
            val enabled = value.toBooleanStrictOrNull() ?: return false
            settings["$section:$key"] = value
            NativePpsspp.nativeSetRewindEnabled(enabled)
            return true
        }
        if ((section == "EmuCoreA" || section == "EmuCoreA/GS") && key == "VsyncEnable") {
            val enabled = value.toBooleanStrictOrNull() ?: return false
            settings["$section:$key"] = value
            NativePpsspp.nativeSetConfig("vsync", if (enabled) "1" else "0")
            return true
        }
        if (section == "EmuCoreA/GS" && (key == "FrameLimitEnable" || key == "TargetFps")) {
            settings["$section:$key"] = value
            pushFpsLimit()
            return true
        }
        if ((section == "EmuCoreA" || section == "EmuCoreA/GS" || section == "EmuCoreA/Display") &&
            key == "AspectRatio"
        ) {
            settings["$section:$key"] = value
            val nativeValue = value.toIntOrNull()?.let(::aspectRatioValue) ?: value
            NativePpsspp.nativeSetConfig("aspect_ratio", nativeValue)
            return true
        }
        settings["$section:$key"] = value
        forwardCoreSetting(section, key, value)
        return true
    }

    /**
     * Translates the app's existing settings into native core config values
     * and forwards them at runtime. Options the native core does not
     * understand are simply ignored.
     */
    private fun forwardCoreSetting(section: String, key: String, value: String) {
        if (section == "EmuCoreA/GS" &&
            (key == "ShaderChainEnabled" || key == "ShaderChainPreset")
        ) {
            pushShaderEffect()
            pushShaderPreset()
        }
        val bool = value.toBooleanStrictOrNull()
        val target: Pair<String, String>? = when ("$section:$key") {
            "EmuCoreA/GS:filter" -> "ppsspp_texture_filtering" to pspTextureFilterName()
            "EmuCoreA:EnableFastMem" ->
                bool?.let { "ppsspp_fast_memory" to if (it) "enabled" else "disabled" }
            "EmuCoreA:EnableEERecompiler" ->
                bool?.let { "ppsspp_cpu_core" to if (it) "JIT" else "IR JIT" }
            "EmuCoreA/Display:Upscale" -> value.toFloatOrNull()?.let {
                val scale = Math.round(it).coerceIn(1, 10)
                "ppsspp_internal_resolution" to "${480 * scale}x${272 * scale}"
            }
            "EmuCoreA/GS:VsyncEnable" ->
                bool?.let { "ppsspp_vsync" to if (it) "enabled" else "disabled" }
            else -> null
        }
        target?.let { (coreKey, coreValue) -> forwardCoreOption(coreKey, coreValue) }
    }

    private fun activeCoreRenderer(): Int =
        runCatching { NativePpsspp.nativeGetActiveRenderer() }
            .getOrDefault(RendererDefaults.toCoreRenderer(requestedRenderer))

    private fun publishPerformanceMetrics(fps: Double, frames: Int, frameNanos: Long,
                                          cpuLoadPercent: Double, speed: Double, targetFps: Double) {
        if (frames <= 0 || !performanceMetricsEnabled) return
        val renderer = RendererDefaults.coreRendererName(activeCoreRenderer())
        val frameMs = frameNanos / frames / 1_000_000.0
        val gpuLoad = if (detailedPerformanceMetrics) GpuLoadReader.loadPercent() else null
        val overlay = buildString {
            append(String.format(Locale.US, "FPS:%.1f | Speed:%.1f%% | Target:%.2f", fps, speed, targetFps))
            if (detailedPerformanceMetrics) {
                // The renderer line must end with " HW |" / " SW |" so the
                // overlay recognises it as the active backend and keeps it on
                // its own bottom line instead of duplicating it inline.
                append('\n').append(renderer).append(" HW |")
                append('\n').append("CPU:Host | ").append(String.format(Locale.US, "%.1f%%", cpuLoadPercent))
                append('\n').append("GPU:Host")
                if (gpuLoad != null) append(String.format(Locale.US, " | %.1f%%", gpuLoad))
                append('\n').append("Res:").append(frameWidth).append('x').append(frameHeight)
                append('\n').append(String.format(Locale.US, "Frame:%.1f ms", frameMs))
            }
        }
        performanceMetricsSnapshot = String.format(Locale.US, "%.3f\n%.3f\n%s", fps, speed, overlay)
    }

    private fun runLoop() {
        var metricsStartNanos = System.nanoTime()
        var metricsFrames = 0
        var metricsFrameTotalNanos = 0L
        var metricsMaxCoreNanos = 0L
        var metricsStartCpuMs = Process.getElapsedCpuTime()
        // Emulated time reported by the core, used for the real speed metric.
        var metricsStartEmuUs = 0L
        var lastEmuUs = 0L
        // Last authoritative emulation speed; reused when a window is not
        // measurable (boot transition, state load, first frames).
        var lastPublishedSpeed = 100.0
        var resetMetrics = true
        var lastRewindNanos = 0L
        var coreExited = false
        // Debug-only diagnostics that do not depend on the performance overlay.
        var diagStartNanos = System.nanoTime()
        var diagFrames = 0
        var diagMaxCoreNanos = 0L
        try {
            while (running) {
                drainFrameTasks()
                if (paused) {
                    resetMetrics = true
                    Thread.sleep(8)
                    continue
                }
                val t0 = System.nanoTime()
                val coreNanos = sessionLock.withLock {
                    if (!running) {
                        null
                    } else {
                        for (port in 0..1) {
                            // Preserve a tap that began and ended between two
                            // guest frames, including cores that poll input
                            // less often than the frontend presents frames.
                            val pressedEdges = pendingPadPressEdges.getAndSet(port, 0)
                            if (pressedEdges != 0) {
                                padEdgeHoldMask[port] = padEdgeHoldMask[port] or pressedEdges
                                padEdgeHoldFrames[port] = 3
                            }
                            val buttons = desiredPadButtons.get(port) and padEdgeHoldMask[port].inv()
                            NativePpsspp.nativeSetPadButtons(port, buttons)
                            if (padEdgeHoldFrames[port] > 0 && --padEdgeHoldFrames[port] == 0) {
                                padEdgeHoldMask[port] = 0
                            }
                            val analog = pendingPadAnalog.get(port)
                            NativePpsspp.nativeSetPadAnalog(
                                port,
                                analog and 0xFF,
                                (analog ushr 8) and 0xFF,
                                (analog ushr 16) and 0xFF,
                                (analog ushr 24) and 0xFF
                            )
                        }
                        if (timeControlMode == 2) {
                            val nowNanos = System.nanoTime()
                            if (nowNanos - lastRewindNanos >= REWIND_STEP_INTERVAL_NANOS) {
                                lastRewindNanos = nowNanos
                                NativePpsspp.nativeRewindStep()
                            }
                        } else {
                            lastRewindNanos = 0L
                        }
                        lastEmuUs = NativePpsspp.nativeRunFrame()
                        if (lastEmuUs < 0L) {
                            // The game exited itself; stop after this frame.
                            coreExited = true
                            running = false
                        }
                        pendingStateRestorePath?.let { restorePath ->
                            // The snapshot is an EmuCoreA container, so extract
                            // the raw core payload first (once).
                            val rawRestorePath = "$restorePath.core"
                            val rawFile = File(rawRestorePath)
                            if (!rawFile.isFile) {
                                if (!extractCorePayload(File(restorePath), rawFile)) {
                                    pendingStateRestorePath = null
                                    File(restorePath).delete()
                                    Log.e(TAG, "Renderer-switch snapshot could not be prepared")
                                }
                            } else if (!isCoreBooted()) {
                                // The core is still booting; frames are not paced
                                // yet, so waiting here does not burn attempts.
                            } else if (NativePpsspp.nativeLoadState(rawRestorePath)) {
                                pendingStateRestorePath = null
                                File(restorePath).delete()
                                rawFile.delete()
                                renderedFirstFrame = false
                                Log.i(TAG, "Renderer-switch snapshot restored")
                            } else if (++pendingStateRestoreAttempts > 300) {
                                pendingStateRestorePath = null
                                File(restorePath).delete()
                                rawFile.delete()
                                Log.e(TAG, "Renderer-switch snapshot restore gave up")
                            }
                        }
                        NativePpsspp.nativeGetFrameSize()
                            ?.takeIf { it.size >= 2 && it[0] > 0 && it[1] > 0 }
                            ?.let {
                                frameWidth = it[0]
                                frameHeight = it[1]
                            }
                        if (!renderedFirstFrame && booted) {
                            renderedFirstFrame = true
                            val startedAt = sessionStartedAtNanos
                            if (startedAt != 0L) {
                                Log.i(TAG, String.format(Locale.US,
                                    "First native frame after %.1f ms",
                                    (System.nanoTime() - startedAt) / 1_000_000.0))
                            }
                        }
                        System.nanoTime() - t0
                    }
                }
                if (coreNanos == null) break
                if (coreExited) {
                    reportFailure("The game exited to the system menu")
                    break
                }
                val frameStartNanos = t0
                val frameNanos = System.nanoTime() - t0

                if (resetMetrics) {
                    metricsStartNanos = frameStartNanos
                    metricsFrames = 0
                    metricsFrameTotalNanos = 0L
                    metricsMaxCoreNanos = 0L
                    metricsStartCpuMs = Process.getElapsedCpuTime()
                    metricsStartEmuUs = lastEmuUs
                    resetMetrics = false
                    continue
                }
                metricsMaxCoreNanos = maxOf(metricsMaxCoreNanos, coreNanos)
                metricsFrames++
                metricsFrameTotalNanos += frameNanos
                val now = frameStartNanos
                if (!performanceMetricsEnabled) {
                    diagFrames++
                    diagMaxCoreNanos = maxOf(diagMaxCoreNanos, coreNanos)
                    if (com.sbro.emucorea.BuildConfig.DEBUG && now - diagStartNanos >= 2_000_000_000L) {
                        val elapsed = now - diagStartNanos
                        Log.d(TAG, "diag fps=%.1f maxCore=%.1fms".format(
                            Locale.US,
                            diagFrames * 1_000_000_000.0 / elapsed,
                            diagMaxCoreNanos / 1_000_000.0))
                        diagStartNanos = now
                        diagFrames = 0
                        diagMaxCoreNanos = 0L
                    }
                    metricsStartNanos = now
                    metricsFrames = 0
                    metricsFrameTotalNanos = 0L
                    metricsStartCpuMs = Process.getElapsedCpuTime()
                    metricsStartEmuUs = lastEmuUs
                } else if (now - metricsStartNanos >= 1_000_000_000L && metricsFrames >= 2) {
                    val elapsed = now - metricsStartNanos
                    val hostFps = (metricsFrames * 1_000_000_000.0 / elapsed).coerceIn(0.0, 999.9)
                    // PPSSPP's own frame statistics. Speed is emulated vblanks
                    // per second (so a genuine slowdown drops below 100%, while
                    // a native 30 fps game keeps 100%); FPS is the frames
                    // actually displayed. If the core has not produced stats
                    // yet, fall back to the host numbers.
                    val displayStats = runCatching { NativePpsspp.nativeGetDisplayStats() }.getOrNull()
                    val vps = displayStats?.getOrNull(0)?.toDouble() ?: 0.0
                    val actualFps = displayStats?.getOrNull(2)?.toDouble() ?: 0.0
                    val targetFps = VBLANK_RATE_HZ
                    val fps = if (actualFps > 0.0) actualFps.coerceIn(0.0, 999.9) else hostFps
                    val speed = emulationSpeedPercent(vps) ?: run {
                        val emuDeltaUs = lastEmuUs - metricsStartEmuUs
                        val plausibleEmuDelta = metricsStartEmuUs > 0L && lastEmuUs > metricsStartEmuUs &&
                            emuDeltaUs * 1_000L <= elapsed * 3L
                        if (plausibleEmuDelta) {
                            (emuDeltaUs * 100_000.0 / elapsed).coerceIn(0.0, 999.9)
                        } else {
                            lastPublishedSpeed
                        }
                    }
                    lastPublishedSpeed = speed
                    val cpuNowMs = Process.getElapsedCpuTime()
                    val cpuDeltaMs = (cpuNowMs - metricsStartCpuMs).coerceAtLeast(0L)
                    val cores = Runtime.getRuntime().availableProcessors().coerceAtLeast(1)
                    val cpuLoad = if (elapsed > 0L) {
                        cpuDeltaMs.toDouble() / (elapsed / 1_000_000.0) / cores * 100.0
                    } else {
                        0.0
                    }
                    publishPerformanceMetrics(fps, metricsFrames, metricsFrameTotalNanos,
                        cpuLoad, speed, targetFps)
                    if (com.sbro.emucorea.BuildConfig.DEBUG) {
                        Log.d(TAG, "pacing fps=%.1f core=%.1fms maxCore=%.1fms".format(
                            Locale.US, fps, metricsFrameTotalNanos / metricsFrames / 1_000_000.0,
                            metricsMaxCoreNanos / 1_000_000.0))
                    }
                    metricsMaxCoreNanos = 0L
                    metricsStartNanos = now
                    metricsStartCpuMs = cpuNowMs
                    metricsStartEmuUs = lastEmuUs
                    metricsFrames = 0
                    metricsFrameTotalNanos = 0L
                }
            }
        } catch (error: InterruptedException) {
            if (running) reportFailure("Emulation worker was interrupted unexpectedly")
            Thread.currentThread().interrupt()
        } catch (error: Throwable) {
            Log.e(TAG, "Emulation frame loop stopped", error)
            reportFailure("Emulation frame loop stopped: ${error.javaClass.simpleName}: ${error.message.orEmpty()}")
        } finally {
            drainFrameTasks()
            running = false
        }
    }

    private fun reportFailure(detail: String) {
        if (_failure.compareAndSet(null, RuntimeFailure(detail))) Log.e(TAG, detail)
    }

    private fun prepareLaunchPath(gamePath: String): String? {
        context?.let(PspStorageBridge::removeLegacyImageCache)
        // content:// games are opened as a raw descriptor at boot time, without
        // copying or mounting them anywhere, so the URI stays the launch path.
        return gamePath
    }

    fun hasDiscMedia(): Boolean = booted || pendingGamePath != null

    private fun isSupportedDiscPath(path: String): Boolean {
        val name = if (path.startsWith("content://")) {
            context?.let { DocumentPathResolver.getDisplayName(it, path) } ?: return false
        } else path
        return PspGameFormats.isSupportedName(name)
    }

    private fun fitRect(containerWidth: Int, containerHeight: Int, contentWidth: Int, contentHeight: Int): Rect {
        val scale = minOf(containerWidth.toFloat() / contentWidth, containerHeight.toFloat() / contentHeight)
        val width = (contentWidth * scale).toInt().coerceAtLeast(1)
        val height = (contentHeight * scale).toInt().coerceAtLeast(1)
        val left = (containerWidth - width) / 2
        val top = (containerHeight - height) / 2
        return Rect(left, top, left + width, top + height)
    }

    // App aspect-ratio preference values (mirrors the display settings UI).
    private const val ASPECT_RATIO_STRETCH = 0
    private const val ASPECT_RATIO_AUTO = 1
    private const val ASPECT_RATIO_CUSTOM = 4
}

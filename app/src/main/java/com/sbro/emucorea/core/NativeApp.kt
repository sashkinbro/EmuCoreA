
package com.sbro.emucorea.core

import android.content.Context
import android.util.Log
import android.view.Surface
import org.json.JSONArray
import java.io.File
import java.io.FileInputStream
import java.lang.ref.WeakReference
import androidx.core.net.toUri
import android.os.ParcelFileDescriptor
import android.os.Handler
import android.os.Looper
import org.json.JSONObject
import java.security.MessageDigest
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

object NativeApp {

    private const val TAG = "NativeApp"
    @JvmStatic
    val hasNativeTools: Boolean

    @JvmStatic
    val loadedCoreLibraryName: String

    @JvmStatic
    val hasNativeCore: Boolean

    private var contextRef: WeakReference<Context>? = null
    private var dataRootOverride: String? = null


    init {
        loadedCoreLibraryName = "emucorea_core"
        hasNativeCore = NativePpsspp.ensureLoaded()
            .also { loaded ->
                if (!loaded) Log.e(TAG, "Unable to load $loadedCoreLibraryName")
            }
        hasNativeTools = false
    }

    @Volatile private var currentGamePath: String = ""
    // The path the user actually launched, used to name save-state files so
    // the writer and every listing agree even when the core needs a prepared
    // (materialized) launch path.
    @Volatile private var saveStateIdentityPath: String? = null
    private val padButtons = intArrayOf(0xFFFF, 0xFFFF)
    private val derivedDpadButtons = intArrayOf(0xFFFF, 0xFFFF)
    private val padAnalogMode = booleanArrayOf(false, false)
    private val analogDpadOptionCache = arrayOfNulls<Boolean>(2)
    private val padAnalogHalfAxes = Array(2) { IntArray(8) }
    private val timeControlHandler = Handler(Looper.getMainLooper())
    private val timeControlButtons = Array(2) { Array(4) { HoldButton() } }
    private val timeControlTapPulse = Array(2) { BooleanArray(2) }
    private val _timeControlMode = MutableStateFlow(0)
    val timeControlMode: StateFlow<Int> = _timeControlMode.asStateFlow()
    private var profilerActive = false
    private var hangTraceActive = false

    @JvmStatic fun initialize(path: String, apiVer: Int) = Unit

    @JvmStatic fun reloadDataRoot(path: String) { dataRootOverride = path.takeIf(String::isNotBlank) }
    @JvmStatic fun setSaveStateIdentityPath(path: String?) {
        saveStateIdentityPath = path?.takeIf(String::isNotBlank)
    }
    @JvmStatic fun setSystemCaBundlePath(path: String) = Unit
    @JvmStatic fun getGameTitle(path: String): String? {
        val fallback = if (path.startsWith("content://")) {
            contextRef?.get()?.let { DocumentPathResolver.getDisplayName(it, path) }
                ?.substringBeforeLast('.')
        } else {
            File(path).nameWithoutExtension
        }.orEmpty().takeIf(String::isNotBlank)
        contextRef?.get()?.let { context ->
            PspGameMetadataReader.read(context, path)?.let { metadata ->
                val title = metadata.title ?: fallback
                val serial = metadata.serial
                if (!serial.isNullOrBlank()) return "${title.orEmpty()}|$serial|$serial"
                if (!title.isNullOrBlank()) return title
            }
        }
        return fallback
    }
    @JvmStatic fun isBiosPath(path: String): Boolean = File(path).let { it.isFile && it.length() == BIOS_SIZE_BYTES }
    /** Takes ownership of [fd] and always closes it before returning. */
    @JvmStatic fun isBiosFd(fd: Int): Boolean = runCatching {
        ParcelFileDescriptor.adoptFd(fd).use { descriptor ->
            FileInputStream(descriptor.fileDescriptor).use { input ->
                var total = 0L
                val buffer = ByteArray(16 * 1024)
                while (true) {
                    val read = input.read(buffer)
                    if (read < 0) break
                    total += read
                    if (total > BIOS_SIZE_BYTES) break
                }
                total == BIOS_SIZE_BYTES
            }
        }
    }.getOrDefault(false)
    @JvmStatic fun setPerformanceMetricsEnabled(visible: Boolean, detailed: Boolean) {
        CoreRuntime.setPerformanceMetricsEnabled(visible, detailed)
    }
    @JvmStatic fun getPerformanceMetricsSnapshot(): String? = CoreRuntime.performanceMetricsSnapshot()
    @JvmStatic fun getDisplayDrawRect(): FloatArray? = CoreRuntime.displayRect()
    @JvmStatic fun getCoreName(): String? = "PPSSPP"
    @JvmStatic fun getCoreVersion(): String? = null
    @JvmStatic fun setAudioOutputGain(volume: Int, muted: Boolean) =
        CoreRuntime.setAudioGain(volume, muted)
    // Audio buffering belongs to the native core (PPSSPP StereoResampler +
    // OpenSL), so the AAudio tuning surface is retained as a no-op.
    @JvmStatic fun setAudioBufferMs(milliseconds: Int) = CoreRuntime.setAudioBufferMs(milliseconds)
    @JvmStatic fun setAudioOutputLatencyMs(milliseconds: Int) = CoreRuntime.setAudioBufferMs(milliseconds)
    @JvmStatic fun setAudioLowLatency(enabled: Boolean) = CoreRuntime.setAudioLowLatency(enabled)
    @JvmStatic fun setRewindEnabled(enabled: Boolean) =
        CoreRuntime.updateSetting("EmuCoreA/GS", "RewindEnabled", enabled.toString())
    @JvmStatic fun queueGsDump(frames: Int) = Unit
    @JvmStatic @Synchronized fun setPadButton(padIndex: Int, index: Int, range: Int, pressed: Boolean) {
        if (padIndex !in 0..1) return
        if (index == PAD_START || index == PAD_SELECT || index == PAD_FAST_FORWARD || index == PAD_REWIND) {
            handleTimeControlHold(padIndex, index, pressed)
            return
        }
        if (index == PAD_ANALOG_TOGGLE) {
            if (pressed) {
                val analog = CoreRuntime.togglePadAnalogMode(padIndex)
                if (analog != null) {
                    padAnalogMode[padIndex] = analog
                    if (analog && derivedDpadButtons[padIndex] != 0xFFFF) {
                        derivedDpadButtons[padIndex] = 0xFFFF
                        CoreRuntime.setPadButtons(padIndex, effectivePadButtons(padIndex))
                    }
                }
            }
            return
        }
        val halfAxis = analogHalfAxisIndex(index)
        if (halfAxis != null) {
            padAnalogHalfAxes[padIndex][halfAxis] = if (pressed) range.coerceIn(0, 255) else 0
            dispatchPadAnalog(padIndex)
            return
        }
        val bit = pspButtonBit(index) ?: return
        padButtons[padIndex] = if (pressed) padButtons[padIndex] and (1 shl bit).inv()
        else padButtons[padIndex] or (1 shl bit)
        CoreRuntime.setPadButtons(padIndex, effectivePadButtons(padIndex))
    }
    @JvmStatic fun setInternetLinkTransportReady(ready: Boolean) = Unit
    @JvmStatic fun resetInternetLinkTransport() = Unit
    @JvmStatic fun pushInternetLinkFrame(frame: ByteArray): Boolean = false
    @JvmStatic fun pollInternetLinkFrame(): ByteArray? = null
    @JvmStatic fun setPadPressureModifierAmount(amountPercent: Int) = Unit
    @JvmStatic fun onHostKeyEvent(keyCode: Int, pressed: Boolean) {
        setPadButton(0, keyCode, 0, pressed)
    }
    @JvmStatic fun onHostMousePosition(x: Float, y: Float) = Unit
    @JvmStatic fun onHostMouseButton(button: Int, pressed: Boolean) = Unit
    @JvmStatic fun onHostMouseWheel(deltaX: Float, deltaY: Float) = Unit
    @JvmStatic fun resetKeyStatus() { resetPadState(0); resetPadState(1) }
    @JvmStatic @Synchronized fun resetPadState(padIndex: Int) {
        if (padIndex !in 0..1) return
        timeControlButtons[padIndex].forEach { it.reset() }
        timeControlTapPulse[padIndex].fill(false)
        updateTimeControl()
        padButtons[padIndex] = 0xFFFF
        derivedDpadButtons[padIndex] = 0xFFFF
        padAnalogHalfAxes[padIndex].fill(0)
        CoreRuntime.setPadButtons(padIndex, 0xFFFF)
        CoreRuntime.setPadAnalog(padIndex, 128, 128, 128, 128)
    }

    @JvmStatic fun setPadAnalogMode(padIndex: Int, enabled: Boolean): Boolean {
        if (padIndex !in 0..1) return false
        val applied = CoreRuntime.setPadAnalogMode(padIndex, enabled)
        if (applied) {
            padAnalogMode[padIndex] = enabled
            if (enabled && derivedDpadButtons[padIndex] != 0xFFFF) {
                derivedDpadButtons[padIndex] = 0xFFFF
                CoreRuntime.setPadButtons(padIndex, effectivePadButtons(padIndex))
            }
        }
        return applied
    }
    @JvmStatic fun setAspectRatio(type: Int) { CoreRuntime.setDisplayAspectRatio(type) }
    @JvmStatic fun renderUpscalemultiplier(value: Float) { setSetting("EmuCoreA/Display", "Upscale", "float", value.toString()) }
    // The software renderer is a CPU rasterizer: the only real internal
    // resolution increase is the 2x "enhanced resolution" buffer.
    @JvmStatic fun getMaxUpscaleMultiplier(renderer: Int): Int =
        if (RendererDefaults.toCoreRenderer(renderer) == RendererDefaults.CORE_SOFTWARE) 1 else UPSCALE_MAX.toInt()
    @JvmStatic fun renderGpu(value: Int) = Unit
    @JvmStatic fun setCustomDriverPath(path: String) {
        CoreRuntime.updateSetting("EmuCoreA/GPU", "CustomDriverPath", path)
    }
    @JvmStatic fun setNativeLibraryDir(path: String) = Unit
    @JvmStatic fun beginSettingsBatch() = Unit
    @JvmStatic fun endSettingsBatch() = Unit
    @JvmStatic fun setSetting(section: String, key: String, type: String, value: String): Boolean =
        CoreRuntime.updateSetting(section, key, value)
    @JvmStatic fun getSetting(section: String, key: String, type: String): String? = CoreRuntime.settings["$section:$key"]
    @JvmStatic fun setCoreOption(key: String, value: String) {
        invalidateAnalogDpadOptionCache(key, value)
        CoreRuntime.setCoreOption(key, value)
    }
    @JvmStatic fun getCoreOption(key: String): String? = CoreRuntime.coreOptionValue(key)
    @JvmStatic fun applyCoreOption(key: String, value: String) {
        invalidateAnalogDpadOptionCache(key, value)
        CoreRuntime.applyCoreOption(key, value)
    }
    @JvmStatic fun setFrameSkip(frames: Int) {
        val clamped = frames.coerceIn(0, 4)
        CoreRuntime.updateSetting("EmuCoreA/GS", "FrameSkip", clamped.toString())
        // PPSSPP's frameskip skips GPU draw work, which is what actually saves
        // frame time. The native core owns the skip counter.
        runCatching {
            CoreRuntime.applyCoreOption(
                "ppsspp_frameskip",
                if (clamped == 0) "disabled" else clamped.toString()
            )
        }
    }
    @JvmStatic fun setDisplayCrop(crop: com.sbro.emucorea.data.DisplayCrop) {
        // Crop/overscan is not exposed by the native core config surface.
        crop.sanitized()
    }
    @JvmStatic fun setFrameLimitEnabled(enabled: Boolean) =
        CoreRuntime.updateSetting("EmuCoreA/GS", "FrameLimitEnable", enabled.toString())
    @JvmStatic fun reloadPatches() = CoreRuntime.reloadCheats()
    @JvmStatic fun loadCheats(path: String) = CoreRuntime.loadCheats(path)
    @JvmStatic fun clearCheats() = CoreRuntime.clearCheats()
    @JvmStatic fun setMemoryCardPath(slot: Int, path: String?) = CoreRuntime.setMemoryCardPath(slot, path)
    @JvmStatic fun setTextureReplacementsPathOverride(path: String?) =
        CoreRuntime.setTextureReplacementsPathOverride(path)
    @JvmStatic fun hasDiscMedia(): Boolean = CoreRuntime.hasDiscMedia()

    // RetroAchievements run on rcheevos inside libemucorea_core through the
    // achievements bridge (nativeAchievements*), reading guest RAM directly
    // from the core's kernel memory base.
    @JvmStatic fun achievementsSetEnabled(enabled: Boolean) {
        if (hasNativeCore) NativePpsspp.nativeAchievementsSetEnabled(enabled)
    }
    @JvmStatic fun achievementsSetHardcore(enabled: Boolean) {
        if (hasNativeCore) NativePpsspp.nativeAchievementsSetHardcore(enabled)
    }
    @JvmStatic fun achievementsSetUnofficial(enabled: Boolean) {
        if (hasNativeCore) NativePpsspp.nativeAchievementsSetUnofficial(enabled)
    }
    @JvmStatic fun achievementsSetEncore(enabled: Boolean) {
        if (hasNativeCore) NativePpsspp.nativeAchievementsSetEncore(enabled)
    }
    @JvmStatic fun achievementsLoginWithPassword(user: String, password: String): String? =
        if (hasNativeCore) NativePpsspp.nativeAchievementsLoginWithPassword(user, password) else null
    @JvmStatic fun achievementsLoginWithToken(user: String, token: String): String? =
        if (hasNativeCore) NativePpsspp.nativeAchievementsLoginWithToken(user, token) else null
    @JvmStatic fun achievementsLogout() {
        if (hasNativeCore) NativePpsspp.nativeAchievementsLogout()
    }
    @JvmStatic fun achievementsLoadGame(path: String) {
        if (hasNativeCore) NativePpsspp.nativeAchievementsLoadGame(path)
    }
    @JvmStatic fun achievementsUnloadGame() {
        if (hasNativeCore) NativePpsspp.nativeAchievementsUnloadGame()
    }
    @JvmStatic fun achievementsPump() {
        if (hasNativeCore) NativePpsspp.nativeAchievementsPump()
    }
    @JvmStatic fun achievementsStateJson(): String =
        if (hasNativeCore) NativePpsspp.nativeAchievementsStateJson() else "{}"
    @JvmStatic fun achievementsAchievementsJson(): String =
        if (hasNativeCore) NativePpsspp.nativeAchievementsAchievementsJson() else "[]"
    @JvmStatic fun achievementsPollEventsJson(): String =
        if (hasNativeCore) NativePpsspp.nativeAchievementsPollEventsJson() else "[]"
    @JvmStatic fun onNativeSurfaceCreated() = Unit
    @JvmStatic fun onNativeSurfaceChanged(surface: Surface, width: Int, height: Int) = CoreRuntime.attachSurface(surface, width, height)
    @JvmStatic fun hasAttachedSurface(surface: Surface, width: Int, height: Int): Boolean =
        CoreRuntime.hasAttachedSurface(surface, width, height)
    @JvmStatic fun onNativeSurfaceDestroyed() = CoreRuntime.detachSurface()
    @JvmStatic fun runVMThread(path: String): Boolean {
        currentGamePath = path
        return CoreRuntime.start(path, biosOnly = path.isBlank())
    }
    @JvmStatic fun restartRenderer(renderer: Int): Boolean = CoreRuntime.restartWithRenderer(renderer)
    @JvmStatic fun changeDisc(path: String): Boolean = CoreRuntime.changeDisc(path)
    @JvmStatic fun runBootSmokeProbe(path: String, steps: Int): Int = 0
    @JvmStatic fun runJitExecutableMemorySmokeTest(): Boolean = false
    @JvmStatic fun runEeFpuDivRoundingSelfTest(): String = "not applicable to R3000A"
    @JvmStatic fun bootElf(path: String): Boolean = false
    @JvmStatic fun bootIrx(path: String): Boolean = false
    @JvmStatic fun pause() = CoreRuntime.pause()
    @JvmStatic fun resume() = CoreRuntime.resume()
    @JvmStatic fun shutdown() = CoreRuntime.shutdown()
    @JvmStatic fun refreshBIOS() = Unit
    @JvmStatic fun hasValidVm(): Boolean = CoreRuntime.isRunning()
    /** Ownership remains after a worker failure until explicit shutdown completes. */
    @JvmStatic fun hasOwnedVm(): Boolean = CoreRuntime.hasSession()
    val runtimeFailure get() = CoreRuntime.failure
    @JvmStatic fun getGameSerial(): String? = extractPspSerial(saveStatePathSource())
    private fun saveStatePathSource(): String =
        saveStateIdentityPath?.takeIf(String::isNotBlank) ?: currentGamePath

    @JvmStatic fun saveStateToSlot(slot: Int): Boolean =
        getSaveStatePathForFile(saveStatePathSource(), slot)?.let(CoreRuntime::saveState) == true

    @JvmStatic fun loadStateFromSlot(slot: Int): Boolean {
        val identityPath = getSaveStatePathForFile(saveStatePathSource(), slot)
        if (identityPath != null && File(identityPath).exists()) {
            return CoreRuntime.loadState(identityPath)
        }

        // Keep saves written with the prepared launch path (before the identity
        // split was fixed) loadable through the legacy name.
        val legacyPath = getSaveStatePathForFile(currentGamePath, slot)
        if (legacyPath != null && legacyPath != identityPath && File(legacyPath).exists()) {
            return CoreRuntime.loadState(legacyPath)
        }

        return identityPath?.let(CoreRuntime::loadState) == true
    }

    @JvmStatic fun getSaveStatePathForFile(path: String, slot: Int): String? {
        if (path.isBlank()) return null
        val context = getContext() ?: return null
        val directory = EmulatorStorage.saveStatesDir(context, dataRootOverride)
        val identity = extractPspSerial(path) ?: path.sha256().take(16).uppercase()
        return File(directory, "$identity.${slot.coerceIn(0, 99).toString().padStart(2, '0')}.rstate").absolutePath
    }
    @JvmStatic fun getCurrentSaveStatePath(slot: Int): String? =
        getSaveStatePathForFile(saveStatePathSource(), slot)
    @JvmStatic fun getSaveStateScreenshot(path: String): ByteArray? = null
    @JvmStatic fun listMemoryCards(): String? {
        val context = getContext() ?: return "[]"
        val directory = EmulatorStorage.memoryCardsDir(context, dataRootOverride).apply { mkdirs() }
        return JSONArray().apply {
            directory.listFiles().orEmpty().filter(File::isFile).forEach { file ->
                put(JSONObject()
                    .put("name", file.name)
                    .put("path", file.absolutePath)
                    .put("modifiedTime", file.lastModified())
                    .put("type", 1)
                    .put("fileType", if (file.length() == PS1_MEMORY_CARD_SIZE_BYTES) 1 else 0)
                    .put("sizeBytes", file.length())
                    .put("formatted", file.length() == PS1_MEMORY_CARD_SIZE_BYTES))
            }
        }.toString()
    }
    @JvmStatic fun createMemoryCard(name: String, type: Int, fileType: Int): Boolean {
        // Memory card images are owned by the native PPSSPP core.
        return false
    }
    @JvmStatic fun convertIsoToChd(inputIsoPath: String): Int = -1
    @JvmStatic fun startJitProfiler() { profilerActive = true }
    @JvmStatic fun stopJitProfiler() { profilerActive = false }
    @JvmStatic fun isJitProfilerActive(): Boolean = profilerActive
    @JvmStatic fun startHangTrace() { hangTraceActive = true }
    @JvmStatic fun stopHangTrace() { hangTraceActive = false }
    @JvmStatic fun isHangTraceActive(): Boolean = hangTraceActive
    @JvmStatic fun setNativeCrashLogFilePath(path: String) = Unit

    @JvmStatic
    fun parseMemoryCardList(raw: String?): List<NativeMemoryCardInfo> {
        if (raw.isNullOrBlank()) return emptyList()
        return runCatching {
            val array = JSONArray(raw)
            buildList {
                for (index in 0 until array.length()) {
                    val item = array.optJSONObject(index) ?: continue
                    add(
                        NativeMemoryCardInfo(
                            name = item.optString("name"),
                            path = item.optString("path"),
                            modifiedTime = item.optLong("modifiedTime"),
                            type = item.optInt("type"),
                            fileType = item.optInt("fileType"),
                            sizeBytes = item.optLong("sizeBytes"),
                            formatted = item.optBoolean("formatted")
                        )
                    )
                }
            }
        }.getOrDefault(emptyList())
    }

    @JvmStatic
    fun initializeOnce(context: Context) {
        contextRef = WeakReference(context.applicationContext)
        val dataRoot = resolveDataRoot(context.applicationContext)
        dataRootOverride = dataRoot
        prepareNativeDataRoot(File(dataRoot))
        CoreRuntime.initialize(context.applicationContext)
    }

    @JvmStatic
    fun getContext(): Context? = contextRef?.get()

    @JvmStatic
    fun onPadVibration(index: Int, largeMotor: Float, smallMotor: Float) {
        GamepadManager.onPadVibration(index, largeMotor, smallMotor)
    }

    @JvmStatic
    fun getPadRumble(index: Int): FloatArray? = null

    @JvmStatic
    fun setCrashContextString(key: String, value: String?) {
        CrashLogger.logContext(key, value)
    }

    @JvmStatic
    fun setCrashContextInt(key: String, value: Int) {
        CrashLogger.logContext(key, value)
    }

    @JvmStatic
    fun setCrashContextBool(key: String, value: Boolean) {
        CrashLogger.logContext(key, value)
    }

    @JvmStatic
    fun logCrashBreadcrumb(message: String) {
        Log.i(TAG, message)
        CrashLogger.logInfo("Native", message)
    }

    @JvmStatic
    fun openContentUri(uriString: String): Int {
        val context = getContext() ?: return -1
        return try {
            val sanitized = uriString.substringBefore('|')
            val descriptor = context.contentResolver.openFileDescriptor(sanitized.toUri(), "r")
            descriptor?.detachFd() ?: -1
        } catch (_: Exception) {
            -1
        }
    }


    /** Root directory that stores native core data such as caches, settings and memory cards. */
    @JvmStatic
    fun dataRoot(context: Context): File = File(resolveDataRoot(context.applicationContext))

    private fun resolveDataRoot(context: Context): String {
        val override = dataRootOverride
        if (!override.isNullOrBlank()) {
            val dir = File(override)
            if (prepareNativeDataRoot(dir)) {
                return dir.absolutePath
            }
            Log.w(TAG, "Configured data root is not writable, falling back to app internal files: $override")
        }

        val external = context.getExternalFilesDir(null)
        if (external != null && prepareNativeDataRoot(external)) {
            return external.absolutePath
        }

        val internal = context.filesDir
        prepareNativeDataRoot(internal)
        return internal.absolutePath
    }

    private fun prepareNativeDataRoot(root: File): Boolean {
        return runCatching {
            if (!root.exists() && !root.mkdirs()) {
                return@runCatching false
            }

            val requiredDirectories = arrayOf(
                File(root, "cache"),
                File(root, "resources"),
                File(root, "inis"),
                File(root, "sstates"),
                File(root, "memcards")
            )
            requiredDirectories.forEach { dir ->
                if (!dir.exists() && !dir.mkdirs()) {
                    return@runCatching false
                }
            }

            val probe = File(root, ".native-write-probe")
            probe.writeText("ok")
            probe.delete()
            true
        }.getOrElse { error ->
            Log.w(TAG, "Native data root is not writable: ${root.absolutePath}", error)
            false
        }
    }

    private fun handleTimeControlHold(padIndex: Int, index: Int, pressed: Boolean) {
        val slot = when (index) {
            PAD_START -> 0
            PAD_SELECT -> 1
            PAD_FAST_FORWARD -> 2
            else -> 3
        }
        val button = timeControlButtons[padIndex][slot]
        if (button.pressed == pressed) return
        if (pressed) {
            val generation = button.press() ?: return
            if (slot < 2 && timeControlTapPulse[padIndex][slot]) {
                timeControlTapPulse[padIndex][slot] = false
                val bit = pspButtonBit(index) ?: return
                padButtons[padIndex] = padButtons[padIndex] or (1 shl bit)
                CoreRuntime.setPadButtons(padIndex, effectivePadButtons(padIndex))
            }
            timeControlHandler.postDelayed({
                synchronized(this) {
                    if (button.activate(generation)) {
                        updateTimeControl()
                    }
                }
            }, TIME_CONTROL_HOLD_MS)
        } else {
            val wasTap = button.release()
            val generation = button.generation
            updateTimeControl()
            if (wasTap && slot < 2) {
                // A tap still reaches PSP as a normal Start/Select press.
                val bit = pspButtonBit(index) ?: return
                timeControlTapPulse[padIndex][slot] = true
                padButtons[padIndex] = padButtons[padIndex] and (1 shl bit).inv()
                CoreRuntime.setPadButtons(padIndex, effectivePadButtons(padIndex))
                timeControlHandler.postDelayed({
                    synchronized(this) {
                        if (button.generation == generation) {
                            timeControlTapPulse[padIndex][slot] = false
                            padButtons[padIndex] = padButtons[padIndex] or (1 shl bit)
                            CoreRuntime.setPadButtons(padIndex, effectivePadButtons(padIndex))
                        }
                    }
                }, TIME_CONTROL_TAP_MS)
            }
        }
    }

    private fun updateTimeControl() {
        val rewind = (0..1).any { pad ->
            timeControlButtons[pad][1].active || timeControlButtons[pad][3].active
        }
        val fastForward = (0..1).any { pad ->
            timeControlButtons[pad][0].active || timeControlButtons[pad][2].active
        }
        val mode = if (rewind) 2 else if (fastForward) 1 else 0
        _timeControlMode.value = mode
        CoreRuntime.setTimeControl(mode)
    }

    private fun pspButtonBit(index: Int): Int? = when (index) {
        109 -> 0  // Select
        106 -> 1  // L3
        107 -> 2  // R3
        108 -> 3  // Start
        19 -> 4   // Up
        22 -> 5   // Right
        20 -> 6   // Down
        21 -> 7   // Left
        102 -> 8  // L1 (CTRL_LTRIGGER, bit 8)
        103 -> 9  // R1 (CTRL_RTRIGGER, bit 9)
        104 -> 10 // L2 (CTRL_L2, bit 10)
        105 -> 11 // R2 (CTRL_R2, bit 11)
        100 -> 12 // Triangle
        97 -> 13  // Circle
        96 -> 14  // Cross
        99 -> 15  // Square
        else -> null
    }

    private fun analogHalfAxisIndex(index: Int): Int? = when (index) {
        110 -> 0 // Left stick up
        111 -> 1 // Left stick right
        112 -> 2 // Left stick down
        113 -> 3 // Left stick left
        120 -> 4 // Right stick up
        121 -> 5 // Right stick right
        122 -> 6 // Right stick down
        123 -> 7 // Right stick left
        else -> null
    }

    private fun mergeHalfAxes(negative: Int, positive: Int): Int {
        val delta = positive - negative
        return if (delta >= 0) 128 + ((delta * 127 + 127) / 255)
        else 128 - (((-delta) * 128 + 127) / 255)
    }

    private fun dispatchPadAnalog(padIndex: Int) {
        val axes = padAnalogHalfAxes[padIndex]
        val lx = mergeHalfAxes(axes[3], axes[1])
        val ly = mergeHalfAxes(axes[0], axes[2])
        val rx = mergeHalfAxes(axes[7], axes[5])
        val ry = mergeHalfAxes(axes[4], axes[6])
        CoreRuntime.setPadAnalog(padIndex, lx, ly, rx, ry)
        updateDerivedDpadButtons(padIndex, lx, ly)
    }

    /**
     * Mirrors the SwanStation "Use Analog Sticks for D-Pad in Digital Mode"
     * option in the frontend input layer. The core option only applies to an
     * emulated DualShock in digital mode, while the app attaches a plain
     * digital pad unless analog mode is requested, so the left stick needs to
     * drive the D-pad buttons here for the setting to have any effect.
     */
    private fun updateDerivedDpadButtons(padIndex: Int, lx: Int, ly: Int) {
        var derived = DPAD_ALL_RELEASED
        if (isAnalogDpadInDigitalModeEnabled(padIndex)) {
            when {
                ly < 128 - ANALOG_DPAD_THRESHOLD -> derived = derived and (1 shl PS1_BUTTON_UP).inv()
                ly > 128 + ANALOG_DPAD_THRESHOLD -> derived = derived and (1 shl PS1_BUTTON_DOWN).inv()
            }
            when {
                lx < 128 - ANALOG_DPAD_THRESHOLD -> derived = derived and (1 shl PS1_BUTTON_LEFT).inv()
                lx > 128 + ANALOG_DPAD_THRESHOLD -> derived = derived and (1 shl PS1_BUTTON_RIGHT).inv()
            }
        }
        if (derivedDpadButtons[padIndex] != derived) {
            derivedDpadButtons[padIndex] = derived
            CoreRuntime.setPadButtons(padIndex, effectivePadButtons(padIndex))
        }
    }

    private fun effectivePadButtons(padIndex: Int): Int =
        padButtons[padIndex] and derivedDpadButtons[padIndex]

    private fun isAnalogDpadInDigitalModeEnabled(padIndex: Int): Boolean {
        if (padAnalogMode[padIndex]) return false
        analogDpadOptionCache[padIndex]?.let { return it }
        val key = "swanstation_Controller${padIndex + 1}_AnalogDPadInDigitalMode"
        val enabled = CoreRuntime.coreOptionValue(key)?.toBooleanStrictOrNull() ?: true
        analogDpadOptionCache[padIndex] = enabled
        return enabled
    }

    private fun invalidateAnalogDpadOptionCache(key: String, value: String) {
        for (index in 0..1) {
            if (key == "swanstation_Controller${index + 1}_AnalogDPadInDigitalMode") {
                analogDpadOptionCache[index] = value.toBooleanStrictOrNull()
            }
        }
    }

    private fun extractPspSerial(value: String): String? {
        val embedded = contextRef?.get()?.let { context ->
            runCatching { PspGameMetadataReader.read(context, value)?.serial }.getOrNull()
        }
        val normalized = (embedded ?: value).uppercase(java.util.Locale.ROOT)
        val match = Regex("\\b([A-Z]{4})[-_. ]?(\\d{5})\\b").find(normalized) ?: return null
        return "${match.groupValues[1]}-${match.groupValues[2]}"
    }

    private fun String.sha256(): String = MessageDigest.getInstance("SHA-256")
        .digest(toByteArray())
        .joinToString("") { byte -> "%02x".format(byte) }

    private const val BIOS_SIZE_BYTES = 512L * 1024L
    private const val PAD_ANALOG_TOGGLE = 125
    private const val PAD_FAST_FORWARD = 126
    private const val PAD_REWIND = 127
    private const val PAD_START = 108
    private const val PAD_SELECT = 109
    private const val TIME_CONTROL_HOLD_MS = 450L
    private const val TIME_CONTROL_TAP_MS = 60L
    private const val PS1_MEMORY_CARD_SIZE_BYTES = 128L * 1024L
    private const val DPAD_ALL_RELEASED = 0xFFFF
    private const val ANALOG_DPAD_THRESHOLD = 64
    private const val PS1_BUTTON_UP = 4
    private const val PS1_BUTTON_RIGHT = 5
    private const val PS1_BUTTON_DOWN = 6
    private const val PS1_BUTTON_LEFT = 7
}

data class NativeMemoryCardInfo(
    val name: String,
    val path: String,
    val modifiedTime: Long,
    val type: Int,
    val fileType: Int,
    val sizeBytes: Long,
    val formatted: Boolean
)

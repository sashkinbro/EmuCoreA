
package com.sbro.emucorea.core

import android.content.Context
import android.util.Log
import android.view.Surface
import java.io.File
import java.lang.ref.WeakReference
import androidx.core.net.toUri
import android.os.Handler
import android.os.Looper
import java.security.MessageDigest
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

object NativeApp {

    private const val TAG = "NativeApp"

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

    @JvmStatic fun reloadDataRoot(path: String) {
        dataRootOverride = path.takeIf(String::isNotBlank)
        val appContext = contextRef?.get() ?: return
        val root = resolveDataRoot(appContext)
        prepareNativeDataRoot(File(root))
        CoreRuntime.setExternalDataDirectory(root)
    }
    @JvmStatic fun setSaveStateIdentityPath(path: String?) {
        saveStateIdentityPath = path?.takeIf(String::isNotBlank)
    }
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
    @JvmStatic fun setPerformanceMetricsEnabled(visible: Boolean, detailed: Boolean) {
        CoreRuntime.setPerformanceMetricsEnabled(visible, detailed)
    }
    @JvmStatic fun getPerformanceMetricsSnapshot(): String? = CoreRuntime.performanceMetricsSnapshot()
    @JvmStatic fun getDisplayDrawRect(): FloatArray? = CoreRuntime.displayRect()
    @JvmStatic fun getCoreName(): String? = "PPSSPP"
    @JvmStatic fun getCoreVersion(): String? = null
    @JvmStatic fun setAudioOutputGain(volume: Int, muted: Boolean) =
        CoreRuntime.setAudioGain(volume, muted)
    // Audio buffering belongs to the native core (PPSSPP StereoResampler plus
    // the AAudio/OpenSL output), so both entry points land on the same knob.
    @JvmStatic fun setAudioOutputLatencyMs(milliseconds: Int) = CoreRuntime.setAudioOutputLatencyMs(milliseconds)
    @JvmStatic fun setAudioLowLatency(enabled: Boolean) = CoreRuntime.setAudioLowLatency(enabled)
    @JvmStatic fun setAudioBackend(backend: Int) = CoreRuntime.setAudioBackend(backend)
    @JvmStatic fun setRewindEnabled(enabled: Boolean) =
        CoreRuntime.updateSetting("EmuCoreA/GS", "RewindEnabled", enabled.toString())
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
    @JvmStatic fun onHostKeyEvent(keyCode: Int, pressed: Boolean) {
        setPadButton(0, keyCode, 0, pressed)
    }
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
    @JvmStatic fun setFrameLimitEnabled(enabled: Boolean) =
        CoreRuntime.updateSetting("EmuCoreA/GS", "FrameLimitEnable", enabled.toString())
    // The software renderer is a CPU rasterizer: the only real internal
    // resolution increase is the 2x "enhanced resolution" buffer.
    @JvmStatic fun getMaxUpscaleMultiplier(renderer: Int): Int =
        if (RendererDefaults.toCoreRenderer(renderer) == RendererDefaults.CORE_SOFTWARE) 1 else UPSCALE_MAX.toInt()
    @JvmStatic fun setCustomDriverPath(path: String) {
        CoreRuntime.updateSetting("EmuCoreA/GPU", "CustomDriverPath", path)
    }
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
    @JvmStatic fun achievementsStateJson(): String =
        if (hasNativeCore) NativePpsspp.nativeAchievementsStateJson() else "{}"
    @JvmStatic fun achievementsAchievementsJson(): String =
        if (hasNativeCore) NativePpsspp.nativeAchievementsAchievementsJson() else "[]"
    @JvmStatic fun achievementsPollEventsJson(): String =
        if (hasNativeCore) NativePpsspp.nativeAchievementsPollEventsJson() else "[]"
    @JvmStatic fun onNativeSurfaceChanged(surface: Surface, width: Int, height: Int) = CoreRuntime.attachSurface(surface, width, height)
    @JvmStatic fun hasAttachedSurface(surface: Surface, width: Int, height: Int): Boolean =
        CoreRuntime.hasAttachedSurface(surface, width, height)
    @JvmStatic fun onNativeSurfaceDestroyed() = CoreRuntime.detachSurface()
    @JvmStatic @JvmOverloads fun runVMThread(path: String, coreOptions: Map<String, String> = emptyMap()): Boolean {
        currentGamePath = path
        return CoreRuntime.start(path, coreOptions)
    }
    @JvmStatic fun restartRenderer(renderer: Int): Boolean = CoreRuntime.restartWithRenderer(renderer)
    @JvmStatic fun changeDisc(path: String): Boolean = CoreRuntime.changeDisc(path)
    @JvmStatic fun pause() = CoreRuntime.pause()
    @JvmStatic fun resume() = CoreRuntime.resume()
    @JvmStatic fun shutdown() = CoreRuntime.shutdown()
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

    @JvmStatic
    fun initializeOnce(context: Context) {
        contextRef = WeakReference(context.applicationContext)
        val dataRoot = resolveDataRoot(context.applicationContext)
        dataRootOverride = dataRoot
        prepareNativeDataRoot(File(dataRoot))
        publishAudioDeviceInfo(context.applicationContext)
        CoreRuntime.initialize(context.applicationContext, dataRoot)
    }

    /**
     * Hands the Android output properties to the core so the output runs at the
     * device's native rate and the resampler sizes its ring for the actual
     * device buffer. Without this the core forces 44.1 kHz and keeps a
     * 1680-sample ring even when the buffer is larger, which underruns on every
     * callback and is heard as a light crackle.
     */
    private fun publishAudioDeviceInfo(context: Context) {
        if (!hasNativeCore) return
        val audioManager = runCatching {
            context.getSystemService(Context.AUDIO_SERVICE) as? android.media.AudioManager
        }.getOrNull()
        val sampleRate = audioManager
            ?.getProperty(android.media.AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)
            ?.toIntOrNull() ?: 0
        val framesPerBuffer = audioManager
            ?.getProperty(android.media.AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER)
            ?.toIntOrNull() ?: 0
        runCatching { NativePpsspp.nativeSetAudioDeviceInfo(sampleRate, framesPerBuffer) }
            .onFailure { Log.w(TAG, "Unable to publish the audio device info", it) }
    }

    @JvmStatic
    fun getContext(): Context? = contextRef?.get()

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
     * Mirrors the "Use Analog Sticks for D-Pad in Digital Mode" option in the
     * frontend input layer. The core option only applies to an emulated pad in
     * digital mode, while the app attaches a plain digital pad unless analog
     * mode is requested, so the left stick needs to drive the D-pad buttons
     * here for the setting to have any effect.
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

    private const val PAD_ANALOG_TOGGLE = 125
    private const val PAD_FAST_FORWARD = 126
    private const val PAD_REWIND = 127
    private const val PAD_START = 108
    private const val PAD_SELECT = 109
    private const val TIME_CONTROL_HOLD_MS = 450L
    private const val TIME_CONTROL_TAP_MS = 60L
    private const val DPAD_ALL_RELEASED = 0xFFFF
    private const val ANALOG_DPAD_THRESHOLD = 64
    private const val PS1_BUTTON_UP = 4
    private const val PS1_BUTTON_RIGHT = 5
    private const val PS1_BUTTON_DOWN = 6
    private const val PS1_BUTTON_LEFT = 7
}

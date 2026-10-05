package com.sbro.emucorea.core

/**
 * Allowed values for the "Target frame rate" setting. 0 means the game's own
 * rate (Auto); any other value limits the emulation rate to that many frames
 * per second, exactly like PPSSPP's frame rate limit. Keeping full emulation
 * speed while drawing fewer frames is what Frameskip is for.
 */
val TARGET_FPS_CHOICES: List<Int> = listOf(0, 25, 30, 40, 50, 60)

data class PerformanceProfileConfig(
    val id: Int,
    val renderer: Int = RendererDefaults.defaultForHardware(),
    val eeCycleRate: Int,
    val eeCycleSkip: Int,
    val hwDownloadMode: Int,
    val fpuCorrectAddSub: Boolean
)

object PerformanceProfiles {
    const val SAFE = 0
    const val FAST = 1

    val safeConfig = PerformanceProfileConfig(
        id = SAFE,
        eeCycleRate = 0,
        eeCycleSkip = 0,
        hwDownloadMode = GsHackDefaults.HW_DOWNLOAD_MODE_DEFAULT,
        fpuCorrectAddSub = true
    )

    val fastConfig = PerformanceProfileConfig(
        id = FAST,
        eeCycleRate = -1,
        eeCycleSkip = 2,
        hwDownloadMode = GsHackDefaults.HW_DOWNLOAD_MODE_DEFAULT,
        fpuCorrectAddSub = false
    )

    fun normalize(profileId: Int): Int {
        return if (profileId == FAST) FAST else SAFE
    }

    fun configFor(profileId: Int): PerformanceProfileConfig {
        return if (normalize(profileId) == FAST) fastConfig else safeConfig
    }
}

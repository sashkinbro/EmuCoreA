package com.sbro.emucorea.core

/**
 * Allowed values for the "Target frame rate" setting. 0 means the game's own
 * rate (Auto); any other value limits the emulation rate to that many frames
 * per second, exactly like PPSSPP's frame rate limit.
 */
val TARGET_FPS_CHOICES: List<Int> = listOf(0, 25, 30, 40, 50, 60)

object PerformanceProfiles {
    const val SAFE = 0
    const val FAST = 1

    fun normalize(profileId: Int): Int {
        return if (profileId == FAST) FAST else SAFE
    }
}

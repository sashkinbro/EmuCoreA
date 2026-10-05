package com.sbro.emucorea.core

/** PSP vblank rate: PPSSPP's emulated vblank clock runs at 59.94 Hz. */
internal const val VBLANK_RATE_HZ = 59.94

/**
 * Emulation speed percentage from PPSSPP's emulated-vblank rate.
 *
 * This is exactly what the standalone app's "Speed" counter shows: a 30 fps
 * game still reports 100% (its emulated clock keeps up), while a host that
 * cannot emulate fast enough reports less. Returns null when no frame
 * statistics are available yet.
 */
internal fun emulationSpeedPercent(vps: Double): Double? =
    if (vps > 0.0) (vps / VBLANK_RATE_HZ * 100.0).coerceIn(0.0, 999.9) else null

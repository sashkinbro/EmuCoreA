package com.sbro.emucorea.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The overlay Speed must be PPSSPP's own metric (emulated vblanks per second),
 * not a static value and not the host framerate. These tests pin the formula
 * so a refactor cannot turn it back into a constant or into fps/60.
 */
class EmulationSpeedMetricTest {
    @Test
    fun fullVblankRateIsHundredPercent() {
        assertEquals(100.0, emulationSpeedPercent(VBLANK_RATE_HZ)!!, 0.05)
    }

    @Test
    fun halfVblankRateIsHalfSpeed() {
        assertEquals(50.0, emulationSpeedPercent(VBLANK_RATE_HZ / 2.0)!!, 0.05)
    }

    @Test
    fun slowerHostDropsBelowFullSpeed() {
        val slow = emulationSpeedPercent(20.0)!!
        assertTrue("expected < 40%, got $slow", slow in 0.0..40.0)
    }

    @Test
    fun noStatisticsMeansNoSpeedYet() {
        assertNull(emulationSpeedPercent(0.0))
        assertNull(emulationSpeedPercent(-10.0))
    }

    @Test
    fun absurdValuesAreClamped() {
        assertEquals(999.9, emulationSpeedPercent(1_000_000.0)!!, 0.0)
    }
}

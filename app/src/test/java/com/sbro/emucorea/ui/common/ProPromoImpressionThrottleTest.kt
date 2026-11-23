package com.sbro.emucorea.ui.common

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ProPromoImpressionThrottleTest {

    @Test
    fun firstImpressionIsLogged_repeatWithinIntervalIsNot() {
        val throttle = ProPromoImpressionThrottle(intervalMs = 1000)
        assertTrue(throttle.tryAcquire("library", 5_000))
        assertFalse(throttle.tryAcquire("library", 5_999))
        assertTrue(throttle.tryAcquire("library", 6_000))
    }

    @Test
    fun placementsAreThrottledIndependently() {
        val throttle = ProPromoImpressionThrottle(intervalMs = 1000)
        assertTrue(throttle.tryAcquire("library", 0))
        assertTrue(throttle.tryAcquire("themes", 10))
        assertFalse(throttle.tryAcquire("themes", 20))
    }
}

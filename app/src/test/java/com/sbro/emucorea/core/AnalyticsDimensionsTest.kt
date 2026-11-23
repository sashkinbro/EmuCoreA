package com.sbro.emucorea.core

import org.junit.Assert.assertEquals
import org.junit.Test

class AnalyticsDimensionsTest {

    @Test
    fun sanitizers_acceptKnownTokensAndMapEverythingElseToUnknown() {
        assertEquals("game", AnalyticsDimensions.sanitizeLaunchType("game"))
        assertEquals("unknown", AnalyticsDimensions.sanitizeLaunchType("/sdcard/Game.iso"))
        assertEquals("native_start", AnalyticsDimensions.sanitizeFailureReason("native_start"))
        assertEquals("unknown", AnalyticsDimensions.sanitizeFailureReason("Some Title"))
        assertEquals("load", AnalyticsDimensions.sanitizeSaveStateAction("load"))
        assertEquals("unknown", AnalyticsDimensions.sanitizeSaveStateAction("delete"))
    }

    @Test
    fun launchType_prefersSmokeThenAutotest() {
        assertEquals("smoke_test", AnalyticsDimensions.launchType(false, true, true))
        assertEquals("autotest", AnalyticsDimensions.launchType(false, false, true))
        assertEquals("game", AnalyticsDimensions.launchType(false, false, false))
    }

    @Test
    fun durationBucket_boundaries() {
        assertEquals("under_1m", AnalyticsDimensions.durationBucket(59_999L))
        assertEquals("1_5m", AnalyticsDimensions.durationBucket(60_000L))
        assertEquals("5_15m", AnalyticsDimensions.durationBucket(5 * 60_000L))
        assertEquals("15_30m", AnalyticsDimensions.durationBucket(15 * 60_000L))
        assertEquals("30_60m", AnalyticsDimensions.durationBucket(30 * 60_000L))
        assertEquals("over_60m", AnalyticsDimensions.durationBucket(60 * 60_000L))
    }

    @Test
    fun upscale_buckets() {
        assertEquals("native", AnalyticsDimensions.upscale(1f))
        assertEquals("2x", AnalyticsDimensions.upscale(2f))
        assertEquals("3x", AnalyticsDimensions.upscale(3f))
        assertEquals("4x_plus", AnalyticsDimensions.upscale(5f))
        assertEquals("native", AnalyticsDimensions.upscale(Float.NaN))
    }
}

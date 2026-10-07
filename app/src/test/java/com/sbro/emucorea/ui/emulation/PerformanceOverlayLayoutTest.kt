package com.sbro.emucorea.ui.emulation

import com.sbro.emucorea.data.PerformanceOverlayMetrics
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class PerformanceOverlayLayoutTest {
    private val snapshot = """
        FPS: 59.94 [P] | VPS: 60.00
        Speed: 100% | Target: 100%
        Vulkan HW
        Frame: 15.20 / 16.67 / 18.40 ms
        Res: 1280x896 NTSC Progressive
        CPU: Snapdragon 8 Gen 3 | 31.5%
        GPU: Adreno 750 | 82.4% (13.73ms)
    """.trimIndent()

    @Test
    fun versionHeaderIsShownOnlyWhenVersionMetricIsSelected() {
        val header = "EmuCoreA-1.0.0 | 42 | v1.2.0"
        val shown = buildPerformanceOverlayLayout(snapshot, PerformanceOverlayMetrics.VERSION, header)
        val hidden = buildPerformanceOverlayLayout(snapshot, PerformanceOverlayMetrics.SPEED, header)

        assertEquals(listOf("EmuCoreA-1.0.0|42|v1.2.0"), shown.mainLines)
        assertEquals(listOf("Speed:100%"), hidden.mainLines)
    }

    @Test
    fun defaultMaskKeepsTheVersionHeaderVisible() {
        val layout = buildPerformanceOverlayLayout(snapshot, PerformanceOverlayMetrics.DEFAULT, "Header")

        assertEquals("Header", layout.mainLines.first())
    }

    @Test
    fun clearingTheVersionMetricHidesTheHeader() {
        val mask = PerformanceOverlayMetrics.DEFAULT and PerformanceOverlayMetrics.VERSION.inv()
        val layout = buildPerformanceOverlayLayout(snapshot, mask, "Header")

        assertTrue(layout.mainLines.none { it == "Header" })
    }
}

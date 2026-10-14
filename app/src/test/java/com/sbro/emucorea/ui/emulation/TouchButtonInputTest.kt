package com.sbro.emucorea.ui.emulation

import androidx.compose.ui.geometry.Rect
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TouchButtonInputTest {
    private class Controls(
        bounds: Map<String, Rect> = linkedMapOf(
            "square" to Rect(0f, 0f, 100f, 100f),
            "triangle" to Rect(50f, 0f, 150f, 100f)
        ),
        held: Set<String> = emptySet()
    ) {
        val events = mutableListOf<Pair<String, Boolean>>()
        val clicks = mutableListOf<String>()
        val haptics = mutableListOf<Boolean>()
        val input = TouchButtonInput(
            hitTargets = { x, y -> touchButtonTargetsAt(bounds, x, y) },
            isTapToHold = { it in held },
            onPressChange = { id, pressed -> events += id to pressed },
            onClick = { clicks += it },
            onHaptic = { _, pressed -> haptics += pressed }
        )
    }

    @Test
    fun `touching an overlap presses and releases both buttons`() {
        val controls = Controls()

        assertTrue(controls.input.down(1, 75f, 50f))

        assertEquals(listOf("square" to true, "triangle" to true), controls.events)
        assertEquals(setOf("square", "triangle"), controls.input.pressedTargets)
        controls.input.up(1, 75f, 50f)
        assertEquals(
            listOf("square" to true, "triangle" to true, "square" to false, "triangle" to false),
            controls.events
        )
        assertTrue(controls.input.pressedTargets.isEmpty())
        assertEquals(listOf(true, false), controls.haptics)
    }

    @Test
    fun `all four fully overlapping face buttons activate regardless of draw order`() {
        val rect = Rect(0f, 0f, 100f, 100f)
        val ids = listOf("square", "triangle", "circle", "cross")
        for (order in listOf(ids, ids.reversed())) {
            val controls = Controls(order.associateWith { rect })
            controls.input.down(1, 50f, 50f)
            assertEquals(ids.toSet(), controls.events.filter { it.second }.map { it.first }.toSet())
            controls.input.up(1, 50f, 50f)
            assertEquals(ids.toSet(), controls.events.filterNot { it.second }.map { it.first }.toSet())
        }
    }

    @Test
    fun `sliding through an overlap only changes buttons entered or left`() {
        val controls = Controls()
        controls.input.down(1, 25f, 50f)
        controls.input.move(1, 75f, 50f)
        controls.input.move(1, 125f, 50f)
        controls.input.move(1, 125f, 50f)

        assertEquals(listOf("square" to true, "triangle" to true, "square" to false), controls.events)
        assertEquals(setOf("triangle"), controls.input.pressedTargets)
        controls.input.up(1, 125f, 50f)
        assertEquals("triangle" to false, controls.events.last())
    }

    @Test
    fun `lifting one finger keeps buttons held by another finger pressed`() {
        val controls = Controls()
        controls.input.down(1, 75f, 50f)
        controls.input.down(2, 25f, 50f)
        controls.input.up(1, 75f, 50f)

        assertEquals(listOf("square" to true, "triangle" to true, "triangle" to false), controls.events)
        assertEquals(setOf("square"), controls.input.pressedTargets)
        controls.input.up(2, 25f, 50f)
        assertEquals("square" to false, controls.events.last())
    }

    @Test
    fun `leaving all bounds releases buttons and reentering presses them again`() {
        val controls = Controls()
        controls.input.down(1, 75f, 50f)
        assertTrue(controls.input.move(1, 200f, 50f))
        assertTrue(controls.input.pressedTargets.isEmpty())
        assertTrue(controls.input.move(1, 75f, 50f))
        assertEquals(setOf("square", "triangle"), controls.input.pressedTargets)
        controls.input.up(1, 200f, 50f)
        assertTrue(controls.input.pressedTargets.isEmpty())
        assertTrue(controls.clicks.isEmpty())
    }

    @Test
    fun `touch starting outside buttons stays available for stick gestures`() {
        val controls = Controls()
        assertFalse(controls.input.down(1, 200f, 50f))
        assertFalse(controls.input.move(1, 75f, 50f))
        assertFalse(controls.input.up(1, 75f, 50f))
        assertTrue(controls.events.isEmpty())
    }

    @Test
    fun `overlapping tap to hold buttons toggle on release and clear on disposal`() {
        val controls = Controls(held = setOf("square", "triangle"))
        controls.input.down(1, 75f, 50f)
        assertTrue(controls.events.isEmpty())
        controls.input.up(1, 75f, 50f)
        assertEquals(listOf("square" to true, "triangle" to true), controls.events)
        controls.input.down(1, 75f, 50f)
        controls.input.up(1, 75f, 50f)
        assertTrue(controls.input.pressedTargets.isEmpty())
        controls.input.down(1, 75f, 50f)
        controls.input.up(1, 75f, 50f)
        controls.input.clear()
        assertTrue(controls.input.pressedTargets.isEmpty())
        assertEquals(listOf("square" to false, "triangle" to false), controls.events.takeLast(2))
    }

    @Test
    fun `release only clicks buttons touched at both start and end`() {
        val controls = Controls()
        controls.input.down(1, 25f, 50f)
        controls.input.move(1, 75f, 50f)
        controls.input.up(1, 75f, 50f)
        assertEquals(listOf("square"), controls.clicks)
    }

    @Test
    fun `sliding onto a held button does not latch it`() {
        val controls = Controls(held = setOf("triangle"))
        controls.input.down(1, 25f, 50f)
        controls.input.move(1, 75f, 50f)
        controls.input.up(1, 75f, 50f)
        assertEquals(listOf("square" to true, "square" to false), controls.events)
        assertTrue(controls.input.pressedTargets.isEmpty())
    }

    @Test
    fun `cancelling releases every active button once without clicking or latching`() {
        val controls = Controls()
        controls.input.down(1, 75f, 50f)
        controls.input.down(2, 75f, 50f)
        controls.input.cancelPointers()
        controls.input.clear()
        assertEquals(
            listOf("square" to true, "triangle" to true, "square" to false, "triangle" to false),
            controls.events
        )
        assertTrue(controls.clicks.isEmpty())
        assertEquals(listOf(true), controls.haptics)
        assertFalse(controls.input.up(1, 75f, 50f))

        val held = Controls(held = setOf("square", "triangle"))
        held.input.down(1, 75f, 50f)
        held.input.cancelPointers()
        assertTrue(held.events.isEmpty())
        assertTrue(held.input.pressedTargets.isEmpty())
    }

    @Test
    fun `cancelling gestures preserves existing latches until disposal`() {
        val controls = Controls(held = setOf("square"))
        controls.input.down(1, 25f, 50f)
        controls.input.up(1, 25f, 50f)
        controls.input.down(2, 75f, 50f)
        controls.input.cancelPointers()
        assertEquals(setOf("square"), controls.input.pressedTargets)
        controls.input.clear()
        assertTrue(controls.input.pressedTargets.isEmpty())
        assertEquals("square" to false, controls.events.last())
    }

    @Test
    fun `button boundaries use the same rectangular bounds as the layout`() {
        val controls = Controls()
        controls.input.down(1, 0f, 0f)
        assertEquals(setOf("square"), controls.input.pressedTargets)
        controls.input.move(1, 100f, 50f)
        assertEquals(setOf("triangle"), controls.input.pressedTargets)
        controls.input.move(1, 150f, 50f)
        assertTrue(controls.input.pressedTargets.isEmpty())
        controls.input.move(1, 75f, 100f)
        assertTrue(controls.input.pressedTargets.isEmpty())
    }
}

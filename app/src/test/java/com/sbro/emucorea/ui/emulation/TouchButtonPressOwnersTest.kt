package com.sbro.emucorea.ui.emulation

import androidx.compose.ui.geometry.Rect
import org.junit.Assert.assertEquals
import org.junit.Test

class TouchButtonPressOwnersTest {
    @Test
    fun `a shared action remains pressed until its last control releases`() {
        val owners = TouchButtonPressOwners()
        val events = mutableListOf<Boolean>()
        val first = owners.handler("triangle") { events += it }
        val second = owners.handler("triangle") { events += it }
        first(true)
        second(true)
        first(false)
        assertEquals(listOf(true), events)
        second(false)
        assertEquals(listOf(true, false), events)
    }

    @Test
    fun `duplicate events do not change an action ownership count`() {
        val owners = TouchButtonPressOwners()
        val events = mutableListOf<Boolean>()
        val handler = owners.handler("cross") { events += it }
        handler(false)
        handler(true)
        handler(true)
        handler(false)
        handler(false)
        assertEquals(listOf(true, false), events)
    }

    @Test
    fun `sliding from a built in button through an overlapping combo preserves its shared action`() {
        val owners = TouchButtonPressOwners()
        val events = mutableListOf<Pair<String, Boolean>>()
        val triangle = owners.handler("triangle") { events += "triangle" to it }
        val comboTriangle = owners.handler("triangle") { events += "triangle" to it }
        val comboCross = owners.handler("cross") { events += "cross" to it }
        val bounds = linkedMapOf(
            "triangle" to Rect(0f, 0f, 100f, 100f),
            "custom:combo" to Rect(50f, 0f, 150f, 100f)
        )
        val input = TouchButtonInput(
            hitTargets = { x, y -> touchButtonTargetsAt(bounds, x, y) },
            isTapToHold = { false },
            onPressChange = { id, pressed ->
                if (id == "triangle") triangle(pressed) else {
                    comboTriangle(pressed)
                    comboCross(pressed)
                }
            },
            onClick = {}, onHaptic = { _, _ -> }
        )
        input.down(1, 25f, 50f)
        input.move(1, 75f, 50f)
        input.move(1, 125f, 50f)
        assertEquals(listOf("triangle" to true, "cross" to true), events)
        input.up(1, 125f, 50f)
        assertEquals(
            listOf("triangle" to true, "cross" to true, "triangle" to false, "cross" to false),
            events
        )
    }
}

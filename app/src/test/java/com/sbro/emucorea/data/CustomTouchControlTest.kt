package com.sbro.emucorea.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class CustomTouchControlTest {

    @Test
    fun decodeOrNullRejectsMissingAndInvalidPayloads() {
        assertNull(CustomTouchControlLibrary.decodeOrNull(null))
        assertNull(CustomTouchControlLibrary.decodeOrNull(" "))
        assertNull(CustomTouchControlLibrary.decodeOrNull("not json"))
        assertEquals(
            0,
            CustomTouchControlLibrary.decodeOrNull("{\"schemaVersion\":2,\"controls\":[]}")
                ?.controls
                ?.size
        )
    }

    @Test
    fun duplicateCreatesOffsetCopyThatKeepsComboAction() {
        val source = CustomTouchControl(
            id = "jump",
            name = "Jump",
            actionId = "cross",
            secondaryActionId = "l1",
            positionX = 0.9f,
            positionY = 0.1f,
            createdAtMillis = 5L,
            updatedAtMillis = 5L
        )

        val copy = source.duplicate(id = "jump-copy", name = "Jump copy", nowMillis = 42L)

        assertEquals("jump-copy", copy.id)
        assertEquals("Jump copy", copy.name)
        assertEquals("cross", copy.actionId)
        assertEquals("l1", copy.secondaryActionId)
        assertEquals(0.95f, copy.positionX)
        assertEquals(0.15f, copy.positionY)
        assertEquals(42L, copy.createdAtMillis)
        assertEquals(42L, copy.updatedAtMillis)
    }

    @Test
    fun duplicateClampsPositionToCanvasBounds() {
        val source = CustomTouchControl(id = "edge", positionX = 1f, positionY = 0.99f)

        val copy = source.duplicate(id = "edge-copy", name = "Edge copy")

        assertEquals(1f, copy.positionX)
        assertEquals(1f, copy.positionY)
    }
}

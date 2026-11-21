package com.sbro.emucorea.xmb

import android.view.KeyEvent
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(manifest = Config.NONE, sdk = [29])
class XmbHostKeyEventTest {

    @After
    fun tearDown() {
        XmbHost.setKeyListener(null)
        XmbHost.swapConfirmButton = false
    }

    @Test
    fun `isActive reflects key listener registration`() {
        assertFalse(XmbHost.isActive())
        XmbHost.setKeyListener { }
        assertTrue(XmbHost.isActive())
        XmbHost.setKeyListener(null)
        assertFalse(XmbHost.isActive())
    }

    @Test
    fun `system back key is not intercepted or swallowed`() {
        val dispatched = mutableListOf<Int>()
        XmbHost.setKeyListener { dispatched += it }

        assertFalse(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK)))
        assertFalse(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK)))
        assertTrue(dispatched.isEmpty())
    }

    @Test
    fun `unhandled system keys pass through on both down and up`() {
        val dispatched = mutableListOf<Int>()
        XmbHost.setKeyListener { dispatched += it }

        assertFalse(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_VOLUME_UP)))
        assertFalse(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_VOLUME_UP)))
        assertFalse(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_VOLUME_DOWN)))
        assertFalse(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_VOLUME_DOWN)))
        assertTrue(dispatched.isEmpty())
    }

    @Test
    fun `dpad keys dispatch on down, repeat on hold, and consume up`() {
        val dispatched = mutableListOf<Int>()
        XmbHost.setKeyListener { dispatched += it }

        assertTrue(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_UP)))
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_DOWN)))
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_LEFT)))
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT)))
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(0L, 0L, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 1)))
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_DPAD_RIGHT)))

        assertEquals(listOf(0, 1, 2, 3, 3), dispatched)
    }

    @Test
    fun `action buttons respect swapConfirmButton and ignore repeats`() {
        val dispatched = mutableListOf<Int>()
        XmbHost.setKeyListener { dispatched += it }

        XmbHost.swapConfirmButton = false
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_A)))
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_B)))
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_Y)))
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(0L, 0L, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_A, 1)))
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(0L, 0L, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_B, 1)))
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(0L, 0L, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_Y, 1)))
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_A)))

        XmbHost.swapConfirmButton = true
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_A)))
        assertTrue(XmbHost.handleKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_B)))

        assertEquals(listOf(4, 5, 6, 5, 4), dispatched)
    }
}

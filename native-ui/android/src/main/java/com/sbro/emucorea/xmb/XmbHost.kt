package com.sbro.emucorea.xmb

import android.view.KeyEvent

/**
 * Host-side hooks the native scene can call. The application installs the
 * listener and launches the selected game; the preview only logs it.
 */
object XmbHost {
    @Volatile
    private var gameListener: ((Int) -> Unit)? = null

    @Volatile
    private var keyListener: ((Int) -> Unit)? = null

    fun setGameListener(listener: ((Int) -> Unit)?) {
        gameListener = listener
    }

    fun setKeyListener(listener: ((Int) -> Unit)?) {
        keyListener = listener
    }

    @JvmStatic
    fun onGameSelected(index: Int) {
        gameListener?.invoke(index)
    }

    /** Dispatches a hardware key to the active XMB host, if any. */
    @JvmStatic
    fun handleKeyEvent(event: KeyEvent): Boolean {
        val listener = keyListener ?: return false
        if (event.action != KeyEvent.ACTION_DOWN) {
            return true
        }
        val code = when (event.keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> 0
            KeyEvent.KEYCODE_DPAD_DOWN -> 1
            KeyEvent.KEYCODE_DPAD_LEFT -> 2
            KeyEvent.KEYCODE_DPAD_RIGHT -> 3
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_BUTTON_A -> 4
            KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_BUTTON_B -> 5
            else -> -1
        }
        if (code < 0) {
            return false
        }
        listener(code)
        return true
    }
}

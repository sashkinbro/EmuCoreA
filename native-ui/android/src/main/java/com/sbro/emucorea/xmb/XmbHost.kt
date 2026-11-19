package com.sbro.emucorea.xmb

import android.os.Handler
import android.os.Looper
import android.view.KeyEvent

/**
 * Host-side hooks the native scene can call. The application installs the
 * listener and launches the selected game; the preview only logs it.
 *
 * UI-affecting hooks may be invoked from the OpenGL render thread (e.g. after
 * a launch flash completes inside nativeDraw), so they are marshaled onto the
 * Android main Looper.
 */
object XmbHost {
    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile
    private var gameListener: ((Int) -> Unit)? = null

    @Volatile
    private var focusListener: ((Int) -> Unit)? = null

    @Volatile
    private var exitListener: (() -> Unit)? = null

    @Volatile
    private var saveListener: ((Int) -> Unit)? = null

    @Volatile
    private var deleteSaveListener: ((Int) -> Unit)? = null

    @Volatile
    private var deleteGameListener: ((Int) -> Unit)? = null

    @Volatile
    private var settingsListener: ((IntArray) -> Unit)? = null

    @Volatile
    private var keyListener: ((Int) -> Unit)? = null

    @Volatile
    var swapConfirmButton: Boolean = false

    fun setGameListener(listener: ((Int) -> Unit)?) {
        gameListener = listener
    }

    fun setFocusListener(listener: ((Int) -> Unit)?) {
        focusListener = listener
    }

    fun setExitListener(listener: (() -> Unit)?) {
        exitListener = listener
    }

    fun setSaveListener(listener: ((Int) -> Unit)?) {
        saveListener = listener
    }

    fun setDeleteSaveListener(listener: ((Int) -> Unit)?) {
        deleteSaveListener = listener
    }

    fun setDeleteGameListener(listener: ((Int) -> Unit)?) {
        deleteGameListener = listener
    }

    fun setSettingsListener(listener: ((IntArray) -> Unit)?) {
        settingsListener = listener
    }

    fun setKeyListener(listener: ((Int) -> Unit)?) {
        keyListener = listener
    }

    @JvmStatic
    fun onGameSelected(index: Int) {
        mainHandler.post { gameListener?.invoke(index) }
    }

    @JvmStatic
    fun onGameFocused(index: Int) {
        mainHandler.post { focusListener?.invoke(index) }
    }

    @JvmStatic
    fun onExitRequested() {
        mainHandler.post { exitListener?.invoke() }
    }

    @JvmStatic
    fun onSaveSelected(index: Int) {
        mainHandler.post { saveListener?.invoke(index) }
    }

    @JvmStatic
    fun onDeleteSave(index: Int) {
        mainHandler.post { deleteSaveListener?.invoke(index) }
    }

    @JvmStatic
    fun onDeleteGame(index: Int) {
        mainHandler.post { deleteGameListener?.invoke(index) }
    }

    @JvmStatic
    fun onSettingsChanged(settings: IntArray) {
        val copy = settings.copyOf()
        mainHandler.post { settingsListener?.invoke(copy) }
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
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER -> 4
            KeyEvent.KEYCODE_BUTTON_A -> if (swapConfirmButton) 5 else 4
            KeyEvent.KEYCODE_BACK -> 5
            KeyEvent.KEYCODE_BUTTON_B -> if (swapConfirmButton) 4 else 5
            KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_MENU,
            KeyEvent.KEYCODE_SPACE -> 6
            else -> -1
        }
        if (code < 0) {
            return false
        }
        listener(code)
        return true
    }
}

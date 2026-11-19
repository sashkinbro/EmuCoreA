package com.sbro.emucorea.xmb

/**
 * Host-side hooks the native scene can call. The application installs the
 * listener and launches the selected game; the preview only logs it.
 */
object XmbHost {
    @Volatile
    private var gameListener: ((Int) -> Unit)? = null

    fun setGameListener(listener: ((Int) -> Unit)?) {
        gameListener = listener
    }

    @JvmStatic
    fun onGameSelected(index: Int) {
        gameListener?.invoke(index)
    }
}

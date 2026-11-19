package com.sbro.emucorea.xmb

/**
 * Entry point to the native XMB renderer. The library is loaded when this
 * object is first touched; nothing here initializes the emulation core.
 */
object XmbNative {
    init {
        System.loadLibrary("emucorea_xmb")
    }

    external fun nativeVersion(): String

    external fun nativeSetResourceRoot(path: String)

    external fun nativeResourceRoot(): String
}

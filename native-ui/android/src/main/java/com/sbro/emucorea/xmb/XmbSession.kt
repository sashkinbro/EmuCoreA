// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

package com.sbro.emucorea.xmb

import android.content.Context
import java.io.File

/**
 * Connects the installed resource tree to the native renderer.
 *
 * [initialize] is safe to call from any thread and from every resume; it only
 * touches the pack installer. The emulation core is not referenced here.
 */
object XmbSession {
    @Volatile
    private var root: String? = null

    fun initialize(context: Context): File = synchronized(this) {
        val directory = XmbResources.ensure(context)
        XmbAudio.initialize(context.applicationContext, directory)
        val path = directory.absolutePath
        if (root != path) {
            XmbNative.nativeSetResourceRoot(path)
            root = path
        }
        directory
    }

    fun resourceRoot(): String? = root

    fun version(): String = XmbNative.nativeVersion()
}

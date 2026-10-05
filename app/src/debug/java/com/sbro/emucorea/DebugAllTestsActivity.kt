// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreA-Proprietary
package com.sbro.emucorea

import android.app.Activity
import android.os.Bundle
import android.util.Log

class DebugAllTestsActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The libretro self-test surface is gone; the native core exposes JNI
        // config, savestates and frame size only.
        Thread(null, Runnable {
            Log.i("AllTests", "PPSSPP native core: self-tests are not available")
            Log.i("AllTests", "ALL DONE")
            runOnUiThread { finish() }
        }, "EmuCoreA-AllTests", (8 * 1024 * 1024).toLong()).start()
    }
}

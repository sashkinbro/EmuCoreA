// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreA-Proprietary
package com.sbro.emucorea

import android.app.Activity
import android.os.Bundle
import android.util.Log
import kotlin.concurrent.thread

/** Debug-build-only entry point retained after the libretro core was removed. */
class DebugJitSelfTestActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        thread(name = "EmuCoreA-JitSelfTest") {
            Log.i(TAG, "PPSSPP native core: JIT self-test is not available")
            runOnUiThread { finish() }
        }
    }

    private companion object {
        const val TAG = "JitSelfTest"
    }
}

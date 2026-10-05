// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreA-Proprietary
package com.sbro.emucorea

import android.app.Activity
import android.os.Bundle
import android.util.Log

class DebugGamesBootActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Thread(null, Runnable {
            Log.i("GamesBoot", "PPSSPP native core: games-boot self-test is not available")
            Log.i("GamesBoot", "FAILED")
            runOnUiThread { finish() }
        }, "GamesBoot", 8 * 1024 * 1024L).start()
    }
}

package com.sbro.emucorea.xmb.preview

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.util.Log
import android.widget.TextView
import com.sbro.emucorea.xmb.XmbSession
import java.io.File

/**
 * Development host for the standalone XMB module.
 *
 * It has no game library and no emulation core: its only job is to prove that
 * the AAR carries the pack, that the first-start installer verifies and
 * unpacks it, and that the native library can be loaded and handed the pack
 * root. Rendering will replace this text once the scene exists.
 */
class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val text = TextView(this).apply {
            textSize = 15f
            setPadding(48, 48, 48, 48)
            setBackgroundColor(Color.rgb(14, 14, 16))
            setTextColor(Color.rgb(225, 225, 230))
            text = "installing native-ui pack..."
        }
        setContentView(text)
        Thread {
            val message = try {
                val directory = XmbSession.initialize(applicationContext)
                val files = directory.walkTopDown().filter { it.isFile }.count()
                val images = File(directory, "images").walkTopDown().count { it.extension == "png" }
                val audio = File(directory, "audio").walkTopDown().count { it.extension == "wav" }
                buildString {
                    appendLine("EmuCoreA XMB preview")
                    appendLine("native ${XmbSession.version()}")
                    appendLine("root ${directory.absolutePath}")
                    appendLine("files $files, images $images, audio $audio")
                    appendLine("manifest ${File(directory, "manifest.json").isFile}")
                    appendLine("native root ${com.sbro.emucorea.xmb.XmbNative.nativeResourceRoot()}")
                }.trim()
            } catch (error: Throwable) {
                Log.e(TAG, "install failed", error)
                "FAILED: $error\n\n${error.stackTraceToString()}"
            }
            Log.i(TAG, message)
            runOnUiThread { text.text = message }
        }.apply { name = "xmb-pack-install" }.start()
    }

    private companion object {
        const val TAG = "XmbPreview"
    }
}

package com.sbro.emucorea.xmb.preview

import android.app.Activity
import android.graphics.Color
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView
import com.sbro.emucorea.xmb.XmbNative
import com.sbro.emucorea.xmb.XmbSession
import com.sbro.emucorea.xmb.XmbSurfaceRenderer
import java.io.File

/**
 * Development host for the standalone XMB module.
 *
 * It has no game library and no emulation core. The GL surface runs the native
 * scene against the installed pack; the overlay only reports what was loaded.
 */
class MainActivity : Activity() {
    private lateinit var surface: GLSurfaceView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        surface = GLSurfaceView(this).apply {
            setEGLContextClientVersion(3)
            preserveEGLContextOnPause = true
            setRenderer(XmbSurfaceRenderer(this@MainActivity))
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
            isFocusable = true
            isFocusableInTouchMode = true
            requestFocus()
            setOnTouchListener { _, event -> handleTouch(event) }
        }
        val status = TextView(this).apply {
            textSize = 11f
            setPadding(24, 12, 24, 12)
            setTextColor(Color.rgb(235, 235, 240))
            setBackgroundColor(Color.argb(150, 0, 0, 0))
            text = "loading native-ui pack..."
        }
        val root = FrameLayout(this).apply {
            addView(surface, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
            addView(status, FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM or Gravity.START,
            ))
        }
        setContentView(root)

        Thread {
            val message = try {
                val directory = XmbSession.initialize(applicationContext)
                val files = directory.walkTopDown().filter { it.isFile }.count()
                buildString {
                    append("native ${XmbSession.version()} · pack ${directory.name.take(8)}")
                    append("\n$files files · ${File(directory, "images").walkTopDown().count { it.extension == "png" }} images")
                    append("\nnative root ${XmbNative.nativeResourceRoot().takeLast(12)}")
                }
            } catch (error: Throwable) {
                Log.e(TAG, "install failed", error)
                "pack failed: $error"
            }
            Log.i(TAG, message.replace('\n', ' '))
            runOnUiThread { status.text = message }
        }.apply { name = "xmb-pack-status" }.start()
    }

    override fun onResume() {
        super.onResume()
        surface.onResume()
    }

    override fun onPause() {
        surface.onPause()
        super.onPause()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
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
            if (code >= 0) {
                XmbNative.nativeKey(code)
                return true
            }
        }
        return super.dispatchKeyEvent(event)
    }

    private var touchDownX = 0f
    private var touchDownY = 0f

    private fun handleTouch(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                touchDownX = event.x
                touchDownY = event.y
                return true
            }
            MotionEvent.ACTION_UP -> {
                val dx = event.x - touchDownX
                val dy = event.y - touchDownY
                val absoluteX = kotlin.math.abs(dx)
                val absoluteY = kotlin.math.abs(dy)
                val code = when {
                    absoluteX < 40f && absoluteY < 40f -> 4
                    absoluteX > absoluteY -> if (dx > 0) 3 else 2
                    else -> if (dy > 0) 1 else 0
                }
                XmbNative.nativeKey(code)
                return true
            }
        }
        return true
    }

    private companion object {
        const val TAG = "XmbPreview"
    }
}

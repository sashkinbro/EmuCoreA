package com.sbro.emucorea.xmb

import android.content.Context
import android.opengl.GLSurfaceView
import android.util.Log
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/** GLSurfaceView renderer that forwards every frame to the native scene. */
class XmbSurfaceRenderer(context: Context) : GLSurfaceView.Renderer {
    private val appContext = context.applicationContext
    private var initialized = false

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        try {
            XmbSession.initialize(appContext)
            initialized = XmbNative.nativeInit()
            if (!initialized) {
                Log.e(TAG, "native scene failed to initialize")
            }
        } catch (error: Throwable) {
            Log.e(TAG, "scene initialization failed", error)
        }
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        XmbNative.nativeResize(width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        if (initialized) {
            XmbNative.nativeDraw()
        }
    }

    private companion object {
        const val TAG = "XmbSurface"
    }
}

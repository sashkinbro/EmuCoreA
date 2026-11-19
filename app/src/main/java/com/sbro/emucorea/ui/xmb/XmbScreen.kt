package com.sbro.emucorea.ui.xmb

import android.opengl.GLSurfaceView
import android.content.Context
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.sbro.emucorea.core.PspGameMetadataReader
import com.sbro.emucorea.data.GameItem
import com.sbro.emucorea.xmb.XmbHost
import com.sbro.emucorea.xmb.XmbNative
import com.sbro.emucorea.xmb.XmbSurfaceRenderer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Compose host for the native PSP XMB. The GL scene owns the rendering; this
 * composable only supplies the library titles and forwards the selected game.
 */
@Composable
fun XmbScreen(
    games: List<GameItem>,
    onGameSelected: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val currentGames = rememberUpdatedState(games)
    val currentSelection = rememberUpdatedState(onGameSelected)
    val iconPaths by produceState(initialValue = emptyArray<String>(), games, context) {
        value = withContext(Dispatchers.IO) {
            games.map { game ->
                runCatching { PspGameMetadataReader.extractIcon0(context, game.path) }.getOrNull().orEmpty()
            }.toTypedArray()
        }
    }
    val currentIcons = rememberUpdatedState(iconPaths)
    val preferences = remember(context) {
        context.getSharedPreferences("xmb_state", Context.MODE_PRIVATE)
    }
    val initialState = remember {
        intArrayOf(
            preferences.getInt("category", 5),
            preferences.getInt("item", 0),
        )
    }

    fun saveState() {
        val state = XmbNative.nativeGetState()
        if (state.size >= 2) {
            preferences.edit()
                .putInt("category", state[0])
                .putInt("item", state[1])
                .apply()
        }
    }

    val lastAppliedTitles = remember { object { var value: Array<String>? = null } }
    val lastAppliedIcons = remember { object { var value: Array<String>? = null } }
    val surface = remember(context) {
        var downX = 0f
        var downY = 0f
        GLSurfaceView(context).apply {
            setEGLContextClientVersion(3)
            preserveEGLContextOnPause = true
            setRenderer(
                XmbSurfaceRenderer(context) {
                    queueEvent {
                        // onReady can arrive after the first recomposition; always
                        // use the latest list, not the empty initial capture.
                        val latestTitles = currentGames.value.map { game -> game.title }.toTypedArray()
                        XmbNative.nativeSetGames(latestTitles, currentIcons.value)
                        XmbNative.nativeSetState(initialState[0], initialState[1])
                    }
                }
            )
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
            isFocusable = true
            isFocusableInTouchMode = true
            requestFocus()
            setOnKeyListener { _, keyCode, event ->
                if (event.action == KeyEvent.ACTION_DOWN) {
                    val code = when (keyCode) {
                        KeyEvent.KEYCODE_DPAD_UP -> 0
                        KeyEvent.KEYCODE_DPAD_DOWN -> 1
                        KeyEvent.KEYCODE_DPAD_LEFT -> 2
                        KeyEvent.KEYCODE_DPAD_RIGHT -> 3
                        KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER,
                        KeyEvent.KEYCODE_BUTTON_A -> 4
                        KeyEvent.KEYCODE_BUTTON_B -> 5
                        else -> -1
                    }
                    if (code >= 0) {
                        XmbNative.nativeKey(code)
                        return@setOnKeyListener true
                    }
                }
                false
            }
            setOnTouchListener { _, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        downX = event.x
                        downY = event.y
                        true
                    }
                    MotionEvent.ACTION_UP -> {
                        val dx = event.x - downX
                        val dy = event.y - downY
                        val ax = kotlin.math.abs(dx)
                        val ay = kotlin.math.abs(dy)
                        val code = when {
                            ax < 40f && ay < 40f -> 4
                            ax > ay -> if (dx > 0) 3 else 2
                            else -> if (dy > 0) 1 else 0
                        }
                        XmbNative.nativeKey(code)
                        true
                    }
                    else -> true
                }
            }
        }
    }

    Log.i("XmbScreen", "library games=${games.size}")

    DisposableEffect(surface, lifecycleOwner) {
        XmbHost.setGameListener { index ->
            if (index in currentGames.value.indices) {
                saveState()
                currentSelection.value(index)
            }
        }
        XmbHost.setKeyListener { XmbNative.nativeKey(it) }
        val window = (context as? android.app.Activity)?.window
        window?.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> surface.onResume()
                Lifecycle.Event.ON_PAUSE -> {
                    saveState()
                    surface.onPause()
                }
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            saveState()
            lifecycleOwner.lifecycle.removeObserver(observer)
            XmbHost.setGameListener(null)
            XmbHost.setKeyListener(null)
        }
    }

    AndroidView(
        factory = { surface },
        modifier = modifier,
        update = {
            // The list can change after a rescan or install; the scene replaces
            // its Game column in place and keeps the rest of the state.
            val latestTitles = currentGames.value.map { game -> game.title }.toTypedArray()
            val latestIcons = currentIcons.value
            val changed = !latestTitles.contentEquals(lastAppliedTitles.value) ||
                !latestIcons.contentEquals(lastAppliedIcons.value)
            if (changed) {
                lastAppliedTitles.value = latestTitles
                lastAppliedIcons.value = latestIcons
                it.queueEvent { XmbNative.nativeSetGames(latestTitles, latestIcons) }
            }
        },
    )
}

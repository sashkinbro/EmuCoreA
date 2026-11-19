package com.sbro.emucorea.ui.xmb

import android.opengl.GLSurfaceView
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.sbro.emucorea.data.GameItem
import com.sbro.emucorea.xmb.XmbHost
import com.sbro.emucorea.xmb.XmbNative
import com.sbro.emucorea.xmb.XmbSurfaceRenderer

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

    val titles = remember(games) { games.map { it.title }.toTypedArray() }
    val surface = remember(context) {
        GLSurfaceView(context).apply {
            setEGLContextClientVersion(3)
            preserveEGLContextOnPause = true
            setRenderer(
                XmbSurfaceRenderer(context) {
                    queueEvent { XmbNative.nativeSetGames(titles) }
                }
            )
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
            isFocusable = true
            isFocusableInTouchMode = true
            requestFocus()
        }
    }

    DisposableEffect(surface, lifecycleOwner) {
        XmbHost.setGameListener { index ->
            if (index in currentGames.value.indices) {
                currentSelection.value(index)
            }
        }
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> surface.onResume()
                Lifecycle.Event.ON_PAUSE -> surface.onPause()
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            XmbHost.setGameListener(null)
        }
    }

    AndroidView(
        factory = { surface },
        modifier = modifier,
        update = {
            // The list can change after a rescan or install; the scene replaces
            // its Game column in place and keeps the rest of the state.
            it.queueEvent { XmbNative.nativeSetGames(currentGames.value.map { game -> game.title }.toTypedArray()) }
        },
    )
}

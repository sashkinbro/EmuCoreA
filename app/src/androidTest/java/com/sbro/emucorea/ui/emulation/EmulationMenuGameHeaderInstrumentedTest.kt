package com.sbro.emucorea.ui.emulation

import android.content.Intent
import android.graphics.Bitmap
import androidx.activity.compose.setContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeOut
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.junit4.v2.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithContentDescription
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.unit.dp
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.sbro.emucorea.RuntimeTestActivity
import com.sbro.emucorea.ui.common.GameCoverArt
import java.io.File
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class EmulationMenuGameHeaderInstrumentedTest {
    @get:Rule val compose = createEmptyComposeRule()

    private fun withMenu(test: (MutableState<EmulationUiState>, String) -> Unit) {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val target = instrumentation.targetContext
        val cover = File.createTempFile("menu-cover-fixture-", ".png", target.cacheDir)
        val bitmap = Bitmap.createBitmap(20, 30, Bitmap.Config.ARGB_8888)
        bitmap.eraseColor(android.graphics.Color.BLUE)
        cover.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        bitmap.recycle()
        val state = mutableStateOf(EmulationUiState(
            showMenu = true, currentGameTitle = "Tekken", currentGameSubtitle = "ULUS-10139",
            currentGameCoverPath = cover.absolutePath
        ))
        val activity = instrumentation.startActivitySync(
            Intent(target, RuntimeTestActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        ) as RuntimeTestActivity
        try {
            instrumentation.runOnMainSync {
                activity.setContent {
                    MaterialTheme {
                        AnimatedVisibility(visible = state.value.showMenu, exit = fadeOut(tween(250))) {
                            val header = rememberEmulationMenuGameHeader(state.value)
                            GameCoverArt(header.coverPath, header.title, Modifier.size(64.dp, 96.dp))
                        }
                    }
                }
            }
            compose.waitUntil(timeoutMillis = 10_000) {
                compose.onAllNodesWithContentDescription("Tekken")
                    .fetchSemanticsNodes(atLeastOneRootRequired = false).isNotEmpty()
            }
            test(state, cover.absolutePath)
        } finally {
            compose.mainClock.autoAdvance = true
            instrumentation.runOnMainSync { activity.finish() }
            instrumentation.waitForIdleSync()
            cover.delete()
        }
    }

    @Test fun coverRemainsVisibleWhileShutdownClearsGameMetadata() = withMenu { state, _ ->
        compose.mainClock.autoAdvance = false
        compose.runOnIdle {
            state.value = state.value.copy(showMenu = false, currentGameTitle = "",
                currentGameSubtitle = "", currentGameCoverPath = null)
        }
        compose.mainClock.advanceTimeByFrame()
        compose.mainClock.advanceTimeBy(100)
        compose.onNodeWithContentDescription("Tekken").assertExists()
        compose.mainClock.advanceTimeBy(300)
        compose.onNodeWithContentDescription("Tekken").assertDoesNotExist()
    }

    @Test fun reopeningMenuUsesTheNextGamesMetadata() = withMenu { state, _ ->
        compose.runOnIdle { state.value = state.value.copy(showMenu = false) }
        compose.waitForIdle()
        compose.runOnIdle {
            state.value = state.value.copy(showMenu = true, currentGameTitle = "Soulcalibur",
                currentGameSubtitle = "ULES-01298", currentGameCoverPath = null)
        }
        compose.onNodeWithText("Soulcalibur").assertExists()
        compose.onNodeWithContentDescription("Tekken").assertDoesNotExist()
    }

    @Test fun visibleMenuAcceptsCoverUpdatesWithoutKeepingStaleArtwork() = withMenu { state, coverPath ->
        compose.runOnIdle { state.value = state.value.copy(currentGameCoverPath = null) }
        compose.onNodeWithContentDescription("Tekken").assertDoesNotExist()
        compose.onNodeWithText("Tekken").assertExists()
        compose.runOnIdle { state.value = state.value.copy(currentGameCoverPath = coverPath) }
        compose.waitUntil(timeoutMillis = 10_000) {
            compose.onAllNodesWithContentDescription("Tekken")
                .fetchSemanticsNodes(atLeastOneRootRequired = false).isNotEmpty()
        }
        compose.onNodeWithContentDescription("Tekken").assertExists()
    }
}

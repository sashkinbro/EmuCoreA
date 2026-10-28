package com.sbro.emucorea.ui.settings

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.material3.Text
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.test.espresso.Espresso
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.sbro.emucorea.ui.common.appScreenEnterTransition
import com.sbro.emucorea.ui.common.appScreenExitTransition
import com.sbro.emucorea.ui.common.appScreenPopEnterTransition
import com.sbro.emucorea.ui.common.appScreenPopExitTransition
import android.app.Activity
import android.content.Context
import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.util.DisplayMetrics
import android.view.WindowManager
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.assertFalse
import com.sbro.emucorea.R
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotDisplayed
import com.sbro.emucorea.RuntimeTestActivity
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.swipeUp
import androidx.compose.ui.unit.dp
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.sbro.emucorea.ui.theme.EmuCoreATheme
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class GpuDriverScreenInstrumentedTest {
    @get:Rule val compose = createAndroidComposeRule<RuntimeTestActivity>()

    @Test fun graphicsSettingsOpenDriverManager() {
        val context = ApplicationProvider.getApplicationContext<Context>()
        var opened = false
        compose.setContent {
            EmuCoreATheme {
                SettingsScreen(initialTab = "graphics", onOpenGpuDriverManager = { opened = true })
            }
        }
        val renderer = compose.onNodeWithText(context.getString(R.string.settings_renderer))
        val driver = compose.onNodeWithText(context.getString(R.string.settings_gpu_driver_manager_title))
        driver.performScrollTo()
        val rendererTop = renderer.fetchSemanticsNode().boundsInRoot.top
        val driverTop = driver.fetchSemanticsNode().boundsInRoot.top
        assertTrue("Driver manager must follow the renderer", rendererTop < driverTop)
        driver.performClick()
        compose.runOnIdle { assertTrue(opened) }
    }

    @Test fun gameMenuDialogCoversTheEntireDisplay() {
        lateinit var activity: Activity
        compose.setContent {
            activity = LocalContext.current as Activity
            EmuCoreATheme { GpuDriverDialog(onDismiss = {}) }
        }
        val previous = activity.requestedOrientation
        try {
            compose.runOnIdle { activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT }
            compose.waitUntil(10_000) { activity.resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT }
            compose.waitForIdle()
            val metrics = DisplayMetrics()
            @Suppress("DEPRECATION")
            (activity.getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.getRealMetrics(metrics)
            val bounds = compose.onNodeWithTag("gpu_driver_list").fetchSemanticsNode().boundsInRoot
            assertEquals(metrics.widthPixels.toFloat(), bounds.width, 1f)
            assertEquals(metrics.heightPixels.toFloat(), bounds.height, 1f)
        } finally {
            compose.runOnIdle { activity.requestedOrientation = previous }
        }
    }

    @Test fun settingsTopBackUsesTheScreenExitTransition() {
        compose.mainClock.autoAdvance = false
        try {
            compose.setContent {
                EmuCoreATheme {
                    val navController = rememberNavController()
                    LaunchedEffect(Unit) { navController.navigate("drivers") }
                    NavHost(
                        navController = navController, startDestination = "settings",
                        enterTransition = { appScreenEnterTransition() },
                        exitTransition = { appScreenExitTransition() },
                        popEnterTransition = { appScreenPopEnterTransition() },
                        popExitTransition = { appScreenPopExitTransition() }
                    ) {
                        composable("settings") { Text("Settings destination") }
                        composable("drivers") { GpuDriverScreen(onBackClick = { navController.popBackStack() }) }
                    }
                }
            }
            compose.mainClock.advanceTimeBy(600)
            compose.onNodeWithTag("gpu_driver_back").performClick()
            compose.mainClock.advanceTimeByFrame()
            compose.onNodeWithTag("gpu_driver_list").assertExists()
            compose.mainClock.advanceTimeBy(500)
            compose.waitForIdle()
            compose.onNodeWithTag("gpu_driver_list").assertDoesNotExist()
            compose.onNodeWithText("Settings destination").assertIsDisplayed()
        } finally { compose.mainClock.autoAdvance = true }
    }

    @Test fun topBackWaitsForTheClosingAnimation() {
        var dismissed = false
        compose.mainClock.autoAdvance = false
        try {
            compose.setContent {
                var open by remember { mutableStateOf(true) }
                EmuCoreATheme {
                    if (open) GpuDriverDialog(onDismiss = { dismissed = true; open = false })
                }
            }
            compose.mainClock.advanceTimeBy(500)
            compose.onNodeWithTag("gpu_driver_back").performClick()
            compose.mainClock.advanceTimeByFrame()
            compose.runOnIdle { assertFalse("Dismissed before the exit transition", dismissed) }
            compose.mainClock.advanceTimeBy(200)
            compose.waitForIdle()
            compose.runOnIdle { assertTrue("Exit transition did not finish", dismissed) }
        } finally { compose.mainClock.autoAdvance = true }
    }

    @Test fun backClosesTheGameMenuDialog() {
        compose.setContent {
            var open by remember { mutableStateOf(true) }
            EmuCoreATheme { if (open) GpuDriverDialog(onDismiss = { open = false }) }
        }
        compose.onNodeWithTag("gpu_driver_list").assertIsDisplayed()
        Espresso.pressBack()
        compose.onNodeWithTag("gpu_driver_list").assertDoesNotExist()
    }

    @Test fun gameMenuDialogCoversLandscapeDisplay() {
        lateinit var activity: Activity
        compose.setContent {
            activity = LocalContext.current as Activity
            EmuCoreATheme { GpuDriverDialog(onDismiss = {}) }
        }
        val previous = activity.requestedOrientation
        try {
            compose.runOnIdle { activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE }
            compose.waitUntil(10_000) { activity.resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE }
            compose.waitForIdle()
            val metrics = DisplayMetrics()
            @Suppress("DEPRECATION")
            (activity.getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.getRealMetrics(metrics)
            val bounds = compose.onNodeWithTag("gpu_driver_list").fetchSemanticsNode().boundsInRoot
            assertEquals(metrics.widthPixels.toFloat(), bounds.width, 1f)
            assertEquals(metrics.heightPixels.toFloat(), bounds.height, 1f)
        } finally {
            compose.runOnIdle { activity.requestedOrientation = previous }
        }
    }

    @Test fun headerScrollsWithTheDriverList() {
        compose.setContent {
            EmuCoreATheme {
                Box(Modifier.width(360.dp).height(300.dp)) {
                    GpuDriverScreen(onBackClick = {})
                }
            }
        }
        compose.onNodeWithTag("gpu_driver_header").assertIsDisplayed()
        compose.onNodeWithTag("gpu_driver_list").performTouchInput { swipeUp() }
        compose.onNodeWithTag("gpu_driver_header").assertIsNotDisplayed()
    }

}

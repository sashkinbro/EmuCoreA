package com.sbro.emucorea.ui.emulation

import android.content.Intent
import androidx.activity.compose.setContent
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.junit4.v2.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.unit.dp
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.sbro.emucorea.R
import com.sbro.emucorea.RuntimeTestActivity
import com.sbro.emucorea.data.TouchControlPressEffect
import com.sbro.emucorea.data.TouchControlVisualStyle
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class TouchButtonGroupInstrumentedTest {
    @get:Rule val compose = createEmptyComposeRule()

    private fun specs(events: MutableList<Pair<String, Boolean>>, held: Boolean = false) = listOf(
        "square" to R.drawable.ic_controller_square_button,
        "triangle" to R.drawable.ic_controller_triangle_button
    ).mapIndexed { index, (id, drawable) ->
        TouchButtonSpec(
            id = id, drawableRes = drawable,
            width = 100.dp, height = 100.dp, x = (index * 50).dp, y = 0.dp,
            shape = CircleShape, tapToHold = held,
            onPressChange = { events += id to it }
        )
    }

    @Composable
    private fun Controls(
        specs: List<TouchButtonSpec>,
        rightStick: Boolean = false,
        onPadInput: (Int, Int, Boolean) -> Unit = { _, _, _ -> }
    ) {
        TouchButtonGroup(
            specs = specs, inputWidth = 220.dp, inputHeight = 100.dp,
            touchscreenRightStick = rightStick, touchscreenRightStickSensitivity = 1f,
            invertRightStick = false, invertRightStickHorizontal = false,
            onPadInput = onPadInput,
            visualStyle = TouchControlVisualStyle.CLASSIC,
            pressEffect = TouchControlPressEffect.GROW,
            onTouchHaptic = {}, modifier = Modifier.testTag("touch-buttons")
        )
    }

    private fun withContent(content: @Composable () -> Unit, test: () -> Unit) {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val activity = instrumentation.startActivitySync(
            Intent(instrumentation.targetContext, RuntimeTestActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        ) as RuntimeTestActivity
        try {
            instrumentation.runOnMainSync { activity.setContent { MaterialTheme { content() } } }
            compose.waitUntil(timeoutMillis = 10_000) {
                compose.onAllNodesWithTag("touch-buttons")
                    .fetchSemanticsNodes(atLeastOneRootRequired = false).isNotEmpty()
            }
            test()
        } finally {
            instrumentation.runOnMainSync { activity.finish() }
            instrumentation.waitForIdleSync()
        }
    }

    @Test
    fun overlapAndSlideDispatchBothButtonsWithoutRepeatingSharedPress() {
        val events = mutableListOf<Pair<String, Boolean>>()
        withContent({ Controls(specs(events)) }) {
            val node = compose.onNodeWithTag("touch-buttons")
            node.performTouchInput { down(Offset(width * 75f / 220f, height / 2f)) }
            compose.runOnIdle { assertEquals(listOf("square" to true, "triangle" to true), events) }
            node.performTouchInput { moveTo(Offset(width * 125f / 220f, height / 2f)) }
            compose.runOnIdle { assertEquals("square" to false, events.last()) }
            node.performTouchInput { up() }
            compose.runOnIdle {
                assertEquals(
                    listOf("square" to true, "triangle" to true, "square" to false, "triangle" to false),
                    events
                )
            }
        }
    }

    @Test
    fun secondFingerKeepsItsButtonPressedWhenOverlapFingerLifts() {
        val events = mutableListOf<Pair<String, Boolean>>()
        withContent({ Controls(specs(events)) }) {
            val node = compose.onNodeWithTag("touch-buttons")
            node.performTouchInput {
                down(0, Offset(width * 75f / 220f, height / 2f))
                down(1, Offset(width * 25f / 220f, height / 2f))
                up(0)
            }
            compose.runOnIdle {
                assertEquals(listOf("square" to true, "triangle" to true, "triangle" to false), events)
            }
            node.performTouchInput { up(1) }
            compose.runOnIdle { assertEquals("square" to false, events.last()) }
        }
    }

    @Test
    fun cancelledTouchReleasesAllOverlappingButtons() {
        val events = mutableListOf<Pair<String, Boolean>>()
        withContent({ Controls(specs(events)) }) {
            compose.onNodeWithTag("touch-buttons").performTouchInput {
                down(Offset(width * 75f / 220f, height / 2f))
                cancel()
            }
            compose.runOnIdle {
                assertEquals(
                    listOf("square" to true, "triangle" to true, "square" to false, "triangle" to false),
                    events
                )
            }
        }
    }

    @Test
    fun removingButtonsDuringTouchReleasesTheirPadInputs() {
        val events = mutableListOf<Pair<String, Boolean>>()
        val buttons = mutableStateOf(specs(events))
        withContent({ Controls(buttons.value) }) {
            compose.onNodeWithTag("touch-buttons").performTouchInput {
                down(Offset(width * 75f / 220f, height / 2f))
            }
            compose.runOnIdle { buttons.value = emptyList() }
            compose.runOnIdle {
                assertEquals(
                    listOf("square" to true, "triangle" to true, "square" to false, "triangle" to false),
                    events
                )
            }
        }
    }

    @Test
    fun overlappingHeldButtonsLatchOnReleaseAndReleaseWhenRemoved() {
        val events = mutableListOf<Pair<String, Boolean>>()
        val buttons = mutableStateOf(specs(events, held = true))
        withContent({ Controls(buttons.value) }) {
            val node = compose.onNodeWithTag("touch-buttons")
            node.performTouchInput { down(Offset(width * 75f / 220f, height / 2f)) }
            compose.runOnIdle { assertEquals(emptyList<Pair<String, Boolean>>(), events) }
            node.performTouchInput { up() }
            compose.runOnIdle {
                assertEquals(listOf("square" to true, "triangle" to true), events)
                buttons.value = emptyList()
            }
            compose.runOnIdle {
                assertEquals(listOf("square" to false, "triangle" to false), events.takeLast(2))
            }
        }
    }

    @Test
    fun changingActionsWhilePressedReleasesTheOriginalActions() {
        val originalEvents = mutableListOf<Pair<String, Boolean>>()
        val replacementEvents = mutableListOf<Pair<String, Boolean>>()
        val buttons = mutableStateOf(specs(originalEvents))
        withContent({ Controls(buttons.value) }) {
            val node = compose.onNodeWithTag("touch-buttons")
            node.performTouchInput { down(Offset(width * 75f / 220f, height / 2f)) }
            compose.runOnIdle { buttons.value = specs(replacementEvents) }
            node.performTouchInput { up() }
            compose.runOnIdle {
                assertEquals(
                    listOf("square" to true, "triangle" to true, "square" to false, "triangle" to false),
                    originalEvents
                )
                assertEquals(emptyList<Pair<String, Boolean>>(), replacementEvents)
            }
            node.performTouchInput {
                down(Offset(width * 75f / 220f, height / 2f))
                up()
            }
            compose.runOnIdle {
                assertEquals(
                    listOf("square" to true, "triangle" to true, "square" to false, "triangle" to false),
                    replacementEvents
                )
            }
        }
    }

    @Test
    fun changingActionsWhileLatchedReleasesTheOriginalActionsOnDisposal() {
        val originalEvents = mutableListOf<Pair<String, Boolean>>()
        val replacementEvents = mutableListOf<Pair<String, Boolean>>()
        val buttons = mutableStateOf(specs(originalEvents, held = true))
        withContent({ Controls(buttons.value) }) {
            compose.onNodeWithTag("touch-buttons").performTouchInput {
                down(Offset(width * 75f / 220f, height / 2f))
                up()
            }
            compose.runOnIdle { buttons.value = specs(replacementEvents, held = true) }
            compose.runOnIdle { buttons.value = emptyList() }
            compose.runOnIdle {
                assertEquals(
                    listOf("square" to true, "triangle" to true, "square" to false, "triangle" to false),
                    originalEvents
                )
                assertEquals(emptyList<Pair<String, Boolean>>(), replacementEvents)
            }
        }
    }

    @Test
    fun overlapTakesPriorityOverRightStickGestures() {
        val events = mutableListOf<Pair<String, Boolean>>()
        val stickEvents = mutableListOf<Triple<Int, Int, Boolean>>()
        withContent({
            Controls(specs(events), rightStick = true) { key, range, pressed ->
                stickEvents += Triple(key, range, pressed)
            }
        }) {
            compose.onNodeWithTag("touch-buttons").performTouchInput {
                down(Offset(width * 75f / 220f, height / 2f))
                moveTo(Offset(width * 80f / 220f, height / 2f))
                up()
            }
            compose.runOnIdle {
                assertEquals(
                    listOf("square" to true, "triangle" to true, "square" to false, "triangle" to false),
                    events
                )
                assertEquals(emptyList<Triple<Int, Int, Boolean>>(), stickEvents)
            }
        }
    }

    @Test
    fun touchOutsideButtonsStillDrivesAndReleasesTheRightStick() {
        val events = mutableListOf<Pair<String, Boolean>>()
        val stickEvents = mutableListOf<Triple<Int, Int, Boolean>>()
        withContent({
            Controls(specs(events), rightStick = true) { key, range, pressed ->
                stickEvents += Triple(key, range, pressed)
            }
        }) {
            val node = compose.onNodeWithTag("touch-buttons")
            node.performTouchInput {
                down(Offset(width * 190f / 220f, height / 2f))
                moveTo(Offset(width * 210f / 220f, height / 2f))
            }
            compose.runOnIdle {
                assertTrue(stickEvents.any {
                    it.first == 121 && it.second > 0 && it.third
                })
                assertEquals(emptyList<Pair<String, Boolean>>(), events)
            }
            node.performTouchInput { up() }
            compose.runOnIdle {
                assertEquals(
                    listOf(
                        Triple(120, 0, false),
                        Triple(121, 0, false),
                        Triple(122, 0, false),
                        Triple(123, 0, false)
                    ),
                    stickEvents.takeLast(4)
                )
            }
        }
    }
}

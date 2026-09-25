package com.sbro.emucorea.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Test

class TouchControlsLayoutPersistenceTest {

    private val layout = TouchControlsLayoutProfile()

    @Test
    fun `custom controls override creates a custom-only profile`() {
        val library = CustomTouchControlLibrary(
            controls = listOf(
                CustomTouchControl(
                    id = "combo",
                    name = "Combo",
                    actionId = "cross",
                    secondaryActionId = "l1"
                )
            )
        )

        val result = null.withCustomTouchControls(
            gameKey = "game.iso",
            gameTitle = "Game",
            gameSerial = "SLUS-12345",
            library = library
        )

        assertEquals(setOf(PER_GAME_CUSTOM_TOUCH_CONTROLS_KEY), result.providedKeys)
        assertEquals("combo", result.customTouchControls?.controls?.single()?.id)
        assertEquals("l1", result.customTouchControls?.controls?.single()?.secondaryActionId)
        assertNull(result.touchControlsLayout)
    }

    @Test
    fun `custom controls are added without losing layout keys`() {
        val existing = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "Game",
            touchControlsLayout = layout,
            providedKeys = setOf("renderer", PER_GAME_TOUCH_CONTROLS_LAYOUT_KEY)
        )

        val result = existing.withCustomTouchControls(
            gameKey = existing.gameKey,
            gameTitle = existing.gameTitle,
            gameSerial = null,
            library = CustomTouchControlLibrary(
                controls = listOf(CustomTouchControl(id = "extra", name = "Extra"))
            )
        )

        assertEquals(
            setOf("renderer", PER_GAME_TOUCH_CONTROLS_LAYOUT_KEY, PER_GAME_CUSTOM_TOUCH_CONTROLS_KEY),
            result.providedKeys
        )
        assertSame(layout, result.touchControlsLayout)
        assertEquals("extra", result.customTouchControls?.controls?.single()?.id)
    }

    @Test
    fun `reset clears both layout and custom control overrides`() {
        val existing = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "Game",
            touchControlsLayout = layout,
            customTouchControls = CustomTouchControlLibrary(
                controls = listOf(CustomTouchControl(id = "extra", name = "Extra"))
            ),
            providedKeys = setOf(
                PER_GAME_TOUCH_CONTROLS_LAYOUT_KEY,
                PER_GAME_CUSTOM_TOUCH_CONTROLS_KEY
            )
        )

        assertNull(existing.withoutTouchControlsLayout())
    }

    @Test
    fun `reset keeps unrelated keys but drops custom controls`() {
        val existing = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "Game",
            renderer = 14,
            touchControlsLayout = layout,
            customTouchControls = CustomTouchControlLibrary(
                controls = listOf(CustomTouchControl(id = "extra", name = "Extra"))
            ),
            providedKeys = setOf(
                "renderer",
                PER_GAME_TOUCH_CONTROLS_LAYOUT_KEY,
                PER_GAME_CUSTOM_TOUCH_CONTROLS_KEY
            )
        )

        val result = requireNotNull(existing.withoutTouchControlsLayout())

        assertEquals(setOf("renderer"), result.providedKeys)
        assertNull(result.touchControlsLayout)
        assertNull(result.customTouchControls)
    }
}

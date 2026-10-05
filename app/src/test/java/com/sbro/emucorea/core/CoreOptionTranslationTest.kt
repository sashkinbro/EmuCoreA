package com.sbro.emucorea.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Guards the contract between the option catalogue shown in the UI and the raw
 * ppsspp_* keys the native core consumes. Every catalogue option must reach
 * nativeSetConfig unchanged; only the renderer-controlled keys are filtered.
 */
class CoreOptionTranslationTest {
    private val managedKeys = setOf("ppsspp_backend", "ppsspp_software_rendering")

    @Test
    fun everyCatalogueOptionIsForwardedToTheNativeCore() {
        PpssppCoreOptions.all().forEach { option ->
            val translated = CoreRuntime.translateCoreOption(option.key, option.defaultValue)
            if (option.key in managedKeys) {
                assertNull("${option.key} must be driven by the renderer controls", translated)
            } else {
                assertEquals(option.key, translated?.first)
                assertEquals(option.defaultValue, translated?.second)
            }
        }
    }

    @Test
    fun everyChoiceOfEveryOptionIsForwarded() {
        PpssppCoreOptions.all().forEach { option ->
            option.choices.forEach { choice ->
                val translated = CoreRuntime.translateCoreOption(option.key, choice.value)
                if (option.key in managedKeys) {
                    assertNull(translated)
                } else {
                    assertEquals(option.key, translated?.first)
                    assertEquals(choice.value, translated?.second)
                }
            }
        }
    }

    @Test
    fun internalResolutionIsForwardedAsACoreKey() {
        assertEquals(
            "ppsspp_internal_resolution" to "480x272",
            CoreRuntime.translateCoreOption("ppsspp_internal_resolution", "480x272")
        )
    }

    @Test
    fun legacySwanStationKeysAreDropped() {
        assertNull(CoreRuntime.translateCoreOption("swanstation_GPU_Backend", "vulkan"))
        assertNull(CoreRuntime.translateCoreOption("swanstation_Display_AspectRatio", "4:3"))
    }

    @Test
    fun nativeShortKeysPassThrough() {
        val shortKeys = listOf(
            "internal_resolution", "frameskip", "auto_frameskip", "frame_duplication",
            "texture_filtering", "texture_scaling_level", "volume", "skip_buffer_effects",
            "fast_memory", "cpu_core", "crop16x9", "vsync", "multi_threading"
        )
        shortKeys.forEach { key ->
            assertEquals(key to "1", CoreRuntime.translateCoreOption(key, "1"))
        }
    }
}

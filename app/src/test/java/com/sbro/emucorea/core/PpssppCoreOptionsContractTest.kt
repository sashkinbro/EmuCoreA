package com.sbro.emucorea.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Structural guarantees for the option catalogue: defaults are real choices,
 * the Vulkan-only features follow the selected renderer, and the in-game menu
 * exposes the same emulation options as the settings screen.
 */
class PpssppCoreOptionsContractTest {
    @Test
    fun everyDefaultValueIsOneOfItsChoices() {
        PpssppCoreOptions.all().forEach { option ->
            assertTrue(
                "${option.key} default '${option.defaultValue}' is not a choice",
                option.choices.any { it.value == option.defaultValue }
            )
        }
    }

    @Test
    fun keysAreUniqueAndPrefixed() {
        val keys = PpssppCoreOptions.all().map { it.key }
        assertEquals(keys.size, keys.toSet().size)
        keys.forEach { assertTrue("bad key $it", it.startsWith("ppsspp_")) }
    }

    @Test
    fun vulkanOnlyFeaturesFollowTheRenderer() {
        val onVulkan = PpssppCoreOptions.graphicsOptions(RendererDefaults.VULKAN).map { it.key }.toSet()
        assertTrue("ppsspp_mulitsample_level" in onVulkan)
        assertTrue("ppsspp_texture_shader" in onVulkan)

        val onOpenGl = PpssppCoreOptions.graphicsOptions(RendererDefaults.OPENGL).map { it.key }.toSet()
        assertFalse("ppsspp_mulitsample_level" in onOpenGl)
        assertFalse("ppsspp_texture_shader" in onOpenGl)

        val inGameGl = PpssppCoreOptions.gameMenuGraphicsOptions(RendererDefaults.OPENGL).map { it.key }.toSet()
        assertFalse("ppsspp_mulitsample_level" in inGameGl)
        assertFalse("ppsspp_texture_shader" in inGameGl)

        val inGameVulkan = PpssppCoreOptions.gameMenuGraphicsOptions(RendererDefaults.VULKAN).map { it.key }
        assertTrue("ppsspp_mulitsample_level" in inGameVulkan)
        assertTrue("ppsspp_texture_shader" in inGameVulkan)
    }

    @Test
    fun inGameMenuExposesEveryEmulationOption() {
        assertEquals(
            PpssppCoreOptions.emulationOptions().map { it.key },
            PpssppCoreOptions.gameMenuEmulationOptions().map { it.key }
        )
    }

    @Test
    fun managedOptionsNeverAppearInAnyList() {
        val managed = setOf("ppsspp_backend", "ppsspp_software_rendering", "ppsspp_internal_resolution")
        val listed = buildSet {
            addAll(PpssppCoreOptions.graphicsOptions(RendererDefaults.VULKAN).map { it.key })
            addAll(PpssppCoreOptions.emulationOptions().map { it.key })
            addAll(PpssppCoreOptions.networkOptions().map { it.key })
            addAll(PpssppCoreOptions.controlsOptions().map { it.key })
            addAll(PpssppCoreOptions.gameMenuGraphicsOptions(RendererDefaults.VULKAN).map { it.key })
        }
        managed.forEach { key -> assertFalse("$key should be hidden", key in listed) }
    }
}

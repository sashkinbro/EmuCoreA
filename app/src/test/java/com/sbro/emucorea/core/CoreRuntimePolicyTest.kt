package com.sbro.emucorea.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertArrayEquals
import org.junit.Test

class CoreRuntimePolicyTest {
    @Test
    fun fourByThreeDisplayRectangleMatchesSelectedViewport() {
        assertArrayEquals(floatArrayOf(240f, 0f, 1680f, 1080f), runtimeDisplayRect(1920, 1080, 2), 0f)
    }

    @Test
    fun customDisplayRectangleUsesTenBySeven() {
        assertArrayEquals(floatArrayOf(200f, 0f, 1200f, 700f), runtimeDisplayRect(1400, 700, 4), 0f)
    }

    @Test
    fun stretchedDisplayRectangleFillsSurface() {
        assertArrayEquals(floatArrayOf(0f, 0f, 1920f, 1080f), runtimeDisplayRect(1920, 1080, 0), 0f)
    }

    @Test
    fun automaticDisplayRectangleKeepsPspPixelAspect() {
        assertArrayEquals(floatArrayOf(7f, 0f, 1912f, 1080f), runtimeDisplayRect(1920, 1080, 1), 0f)
    }
    @Test
    fun enabledDefaultLimiterUsesNormalPspRateInsteadOfUnlimited() {
        assertEquals(60, runtimeFpsLimit(enabled = true, target = 0))
    }

    @Test
    fun disablingLimiterUsesUnlimitedEvenWithACustomTarget() {
        assertEquals(0, runtimeFpsLimit(enabled = false, target = 30))
    }

    @Test
    fun customLimitIsClampedToSupportedRange() {
        assertEquals(30, runtimeFpsLimit(enabled = true, target = 30))
        assertEquals(20, runtimeFpsLimit(enabled = true, target = 1))
        assertEquals(120, runtimeFpsLimit(enabled = true, target = 240))
    }

    @Test
    fun appTexturePreferenceOverridesCatalogueDefaultAtBoot() {
        assertEquals("enabled", runtimeCoreOptionValue(
            key = "ppsspp_texture_replacement", stored = null, default = "disabled",
            textureFilter = "Auto", textureReplacement = "true"
        ))
    }

    @Test
    fun explicitCoreTextureOverrideTakesPrecedenceOverAppPreference() {
        assertEquals("disabled", runtimeCoreOptionValue(
            key = "ppsspp_texture_replacement", stored = "disabled", default = "disabled",
            textureFilter = "Auto", textureReplacement = "true"
        ))
    }

    @Test
    fun unsetAppTexturePreferenceUsesCatalogueDefault() {
        assertEquals("disabled", runtimeCoreOptionValue(
            key = "ppsspp_texture_replacement", stored = null, default = "disabled",
            textureFilter = "Auto", textureReplacement = null
        ))
    }

    @Test
    fun sessionBootOptionsOverrideGlobalOptionsWithoutMutatingThem() {
        val globals = mapOf("ppsspp_cpu_core" to "JIT", "ppsspp_language" to "English")
        val effective = runtimeStartCoreOptions(
            defaults = mapOf("ppsspp_cpu_core" to "JIT", "ppsspp_language" to "Automatic"),
            global = globals,
            session = mapOf("ppsspp_cpu_core" to "Interpreter", "ppsspp_language" to "Japanese"),
            textureFilter = "Auto", textureReplacement = null
        )
        assertEquals("Interpreter", effective["ppsspp_cpu_core"])
        assertEquals("Japanese", effective["ppsspp_language"])
        assertEquals(mapOf("ppsspp_cpu_core" to "JIT", "ppsspp_language" to "English"), globals)
    }

    @Test
    fun sessionTextureSelectionOverridesAppAndGlobalSelections() {
        val effective = runtimeStartCoreOptions(
            defaults = mapOf("ppsspp_texture_replacement" to "disabled"),
            global = mapOf("ppsspp_texture_replacement" to "disabled"),
            session = mapOf("ppsspp_texture_replacement" to "enabled"),
            textureFilter = "Auto", textureReplacement = "false"
        )
        assertEquals("enabled", effective["ppsspp_texture_replacement"])
    }
}

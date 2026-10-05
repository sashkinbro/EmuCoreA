package com.sbro.emucorea.core

import org.junit.Assert.assertEquals
import org.junit.Test

/** Android UI renderer values must translate to the native core contract. */
class RendererDefaultsTest {
    @Test
    fun androidValuesMapToTheCoreContract() {
        assertEquals(RendererDefaults.CORE_OPENGL, RendererDefaults.toCoreRenderer(RendererDefaults.OPENGL, false))
        assertEquals(RendererDefaults.CORE_VULKAN, RendererDefaults.toCoreRenderer(RendererDefaults.VULKAN, false))
        assertEquals(RendererDefaults.CORE_SOFTWARE, RendererDefaults.toCoreRenderer(RendererDefaults.SOFTWARE, false))
    }

    @Test
    fun unknownValuesNormalizeToTheDefault() {
        assertEquals(RendererDefaults.VULKAN, RendererDefaults.normalizeAndroidRenderer(RendererDefaults.AUTO, false))
        assertEquals(RendererDefaults.VULKAN, RendererDefaults.normalizeAndroidRenderer(999, false))
    }

    @Test
    fun namesMatchTheOverlayLabels() {
        assertEquals("Vulkan", RendererDefaults.coreRendererName(RendererDefaults.CORE_VULKAN))
        assertEquals("OpenGL", RendererDefaults.coreRendererName(RendererDefaults.CORE_OPENGL))
        assertEquals("Software", RendererDefaults.coreRendererName(RendererDefaults.CORE_SOFTWARE))
    }
}

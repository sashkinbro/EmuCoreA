package com.sbro.emucorea.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class DeviceGpuInfoResolverTest {
    @Test
    fun `renderer wins over the SoC catalog`() {
        val snapshot = DeviceGpuInfoResolver.resolve(
            renderer = "Adreno (TM) 610",
            socName = "Snapdragon 8 Gen 3"
        )
        assertEquals(GpuInfoSource.GL_RENDERER, snapshot.source)
        assertEquals("Adreno 610", snapshot.model?.displayName)
        assertEquals(GpuTier.RED, snapshot.tier)
    }

    @Test
    fun `falls back to the SoC catalog when the renderer is unavailable`() {
        val snapshot = DeviceGpuInfoResolver.resolve(
            renderer = null,
            socName = "Snapdragon 8 Gen 3"
        )
        assertEquals(GpuInfoSource.SOC_CATALOG, snapshot.source)
        assertEquals("Adreno 750", snapshot.model?.displayName)
        assertEquals(GpuTier.GREEN, snapshot.tier)
    }

    @Test
    fun `falls back to the SoC catalog for emulator renderer strings`() {
        val snapshot = DeviceGpuInfoResolver.resolve(
            renderer = "SwiftShader",
            socName = "Dimensity 9400"
        )
        assertEquals(GpuInfoSource.SOC_CATALOG, snapshot.source)
        assertEquals("Immortalis-G925 MC12", snapshot.model?.displayName)
        assertEquals(GpuTier.GREEN, snapshot.tier)
    }

    @Test
    fun `unknown hardware fails open`() {
        val unknownSoc = DeviceGpuInfoResolver.resolve(renderer = null, socName = "Unknown SoC")
        assertEquals(GpuInfoSource.UNKNOWN, unknownSoc.source)
        assertNull(unknownSoc.model)
        assertEquals(GpuTier.UNKNOWN, unknownSoc.tier)

        val nothing = DeviceGpuInfoResolver.resolve(renderer = null, socName = null)
        assertEquals(GpuInfoSource.UNKNOWN, nothing.source)
        assertNull(nothing.model)
        assertEquals(GpuTier.UNKNOWN, nothing.tier)
    }

    @Test
    fun `renderer only resolves without any SoC name`() {
        val snapshot = DeviceGpuInfoResolver.resolve(
            renderer = "Mali-G52 MC2",
            socName = null
        )
        assertEquals(GpuInfoSource.GL_RENDERER, snapshot.source)
        assertEquals("Mali-G52 MC2", snapshot.model?.displayName)
        assertEquals(GpuTier.RED, snapshot.tier)
    }

    @Test
    fun `merges the catalog core count when the renderer omits it`() {
        val singleCore = DeviceGpuInfoResolver.resolve(
            renderer = "ARM, Mali-G57",
            socName = "Unisoc T606/T612"
        )
        assertEquals(GpuInfoSource.GL_RENDERER, singleCore.source)
        assertEquals("Mali-G57 MP1", singleCore.model?.displayName)
        assertEquals(GpuTier.RED, singleCore.tier)

        val fourCores = DeviceGpuInfoResolver.resolve(
            renderer = "Mali-G57",
            socName = "Unisoc T820"
        )
        assertEquals("Mali-G57 MC4", fourCores.model?.displayName)
        assertEquals(GpuTier.YELLOW, fourCores.tier)

        val exynos = DeviceGpuInfoResolver.resolve(
            renderer = "Mali-G68",
            socName = "Exynos 1380"
        )
        assertEquals("Mali-G68 MP5", exynos.model?.displayName)
        assertEquals(GpuTier.YELLOW, exynos.tier)
    }

    @Test
    fun `keeps the renderer core count and never merges across models`() {
        val rendererWins = DeviceGpuInfoResolver.resolve(
            renderer = "Mali-G57 MC2",
            socName = "Unisoc T606/T612"
        )
        assertEquals("Mali-G57 MC2", rendererWins.model?.displayName)
        assertEquals(GpuTier.YELLOW, rendererWins.tier)

        val differentModel = DeviceGpuInfoResolver.resolve(
            renderer = "Mali-G57",
            socName = "Exynos 990"
        )
        assertEquals("Mali-G57", differentModel.model?.displayName)
        assertEquals(GpuTier.YELLOW, differentModel.tier)

        val noCatalog = DeviceGpuInfoResolver.resolve(
            renderer = "Mali-G57",
            socName = "Unknown SoC"
        )
        assertEquals("Mali-G57", noCatalog.model?.displayName)
        assertEquals(GpuTier.YELLOW, noCatalog.tier)
    }
}

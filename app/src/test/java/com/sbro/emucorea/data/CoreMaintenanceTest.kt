package com.sbro.emucorea.data

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CoreMaintenanceTest {

    @Test
    fun `reset clears regenerable files and keeps user data`() {
        val root = Files.createTempDirectory("core-maintenance").toFile()
        try {
            val inis = File(root, "inis").apply { mkdirs() }
            File(inis, "core.ini").writeText("old settings")
            val cache = File(root, "cache").apply { mkdirs() }
            File(cache, "baked_shaders.bin").writeText("shaders")
            File(File(cache, "achievement_images").apply { mkdirs() }, "badge.png").writeText("badge")

            val memcards = File(root, "memcards").apply { mkdirs() }
            val memoryCard = File(memcards, "card.bin").apply { writeText("memory card") }
            val sstates = File(root, "sstates").apply { mkdirs() }
            val saveState = File(sstates, "game.p2s").apply { writeText("state") }
            val resources = File(root, "resources").apply { mkdirs() }
            val patches = File(resources, "patches.zip").apply { writeText("patches") }

            val deletedFiles = clearRegenerableCoreState(root)

            assertEquals(3, deletedFiles)
            assertTrue(inis.isDirectory)
            assertFalse(File(inis, "core.ini").exists())
            assertTrue(cache.isDirectory)
            assertFalse(File(cache, "baked_shaders.bin").exists())
            assertFalse(File(cache, "achievement_images").exists())
            assertTrue(memoryCard.isFile)
            assertEquals("memory card", memoryCard.readText())
            assertTrue(saveState.isFile)
            assertTrue(patches.isFile)
        } finally {
            root.deleteRecursively()
        }
    }

    @Test
    fun `reset clears shader caches from the previous core`() {
        val root = Files.createTempDirectory("core-maintenance-gpu").toFile()
        try {
            val shaderCache = File(root, "PSP/SYSTEM/CACHE").apply { mkdirs() }
            val vulkanCache = File(shaderCache, "ULUS10285.vkshadercache").apply { writeText("old vulkan") }
            val openGlCache = File(shaderCache, "ULUS10285.glshadercache").apply { writeText("old opengl") }
            val pipelineDir = File(shaderCache, "pipelines").apply { mkdirs() }
            val pipelineCache = File(pipelineDir, "pipeline.bin").apply { writeText("old pipeline") }
            File(root, "PSP/SYSTEM").mkdirs()
            val systemSettings = File(root, "PSP/SYSTEM/settings.ini").apply { writeText("keep") }

            val deletedFiles = clearRegenerableCoreState(root)

            assertEquals(3, deletedFiles)
            assertTrue(shaderCache.isDirectory)
            assertFalse(vulkanCache.exists())
            assertFalse(openGlCache.exists())
            assertFalse(pipelineDir.exists())
            assertEquals("keep", systemSettings.readText())
        } finally {
            root.deleteRecursively()
        }
    }

    @Test
    fun `reset tolerates a data root without regenerable directories`() {
        val root = Files.createTempDirectory("core-maintenance-empty").toFile()
        try {
            File(root, "memcards").mkdirs()
            assertEquals(0, clearRegenerableCoreState(root))
        } finally {
            root.deleteRecursively()
        }
    }
}

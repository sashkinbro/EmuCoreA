package com.sbro.emucorea.core

import java.io.File
import java.io.IOException
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class CoreRuntimeRendererSnapshotTest {
    @Test
    fun consecutiveRestartsOwnDistinctSnapshotsAndKeepThePendingGamePosition() {
        val directory = Files.createTempDirectory("renderer-restart").toFile()
        try {
            val original = runtimeRendererSnapshot(directory, null)
            original.writeText("saved game position")
            val next = runtimeRendererSnapshot(directory, original)
            assertNotEquals(original.absolutePath, next.absolutePath)
            original.delete() // Shutting down the earlier restart releases its snapshot.
            assertEquals("saved game position", next.readText())
            val third = runtimeRendererSnapshot(directory, next)
            next.delete()
            assertEquals("saved game position", third.readText())
        } finally {
            directory.deleteRecursively()
        }
    }

    @Test
    fun missingPendingSnapshotAbortsWithoutLeavingAnEmptyReplacement() {
        val directory = Files.createTempDirectory("renderer-restart").toFile()
        try {
            var failed = false
            try {
                runtimeRendererSnapshot(directory, File(directory, "missing.sav"))
            } catch (_: IOException) {
                failed = true
            }
            assertEquals(true, failed)
            assertFalse(directory.listFiles().orEmpty().any())
        } finally {
            directory.deleteRecursively()
        }
    }
}

package com.sbro.emucorea.core

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The renderer switch saves a state and restores it after the reboot. That
 * snapshot is an EmuCoreA container (8-byte magic + version), while the native
 * core only understands the raw stream inside it. Skipping the extraction made
 * the restore silently fail and the game started from the beginning.
 */
class CoreRuntimeStateContainerTest {
    private val payload = "SaveStart\u0000raw-core-stream".toByteArray()

    private fun containerBytes(): ByteArray {
        val magic = 0x54534345
        val version = 1
        val header = ByteArray(8)
        for (index in 0 until 4) {
            header[index] = (magic ushr (index * 8)).toByte()
            header[4 + index] = (version ushr (index * 8)).toByte()
        }
        return header + payload
    }

    @Test
    fun containerHeaderIsStripped() {
        val dir = Files.createTempDirectory("emucorea-state").toFile()
        val source = File(dir, "slot.rstate").apply { writeBytes(containerBytes()) }
        val destination = File(dir, "slot.raw")

        assertTrue(CoreRuntime.extractCorePayload(source, destination))
        assertEquals(payload.size, destination.length().toInt())
        assertTrue(destination.readBytes().contentEquals(payload))
    }

    @Test
    fun rawStateIsCopiedVerbatim() {
        val dir = Files.createTempDirectory("emucorea-state").toFile()
        val source = File(dir, "legacy.rstate").apply { writeBytes(payload) }
        val destination = File(dir, "legacy.raw")

        assertTrue(CoreRuntime.extractCorePayload(source, destination))
        assertTrue(destination.readBytes().contentEquals(payload))
    }
}

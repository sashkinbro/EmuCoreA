package com.sbro.emucorea.core

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class GpuDriverValidationTest {
    @Test
    fun `accepts a file with ELF magic`() {
        val file = File.createTempFile("driver", ".so")
        try {
            file.writeBytes(byteArrayOf(0x7F, 'E'.code.toByte(), 'L'.code.toByte(), 'F'.code.toByte(), 2, 1, 1))
            assertTrue(isValidDriverLibrary(file))
        } finally {
            file.delete()
        }
    }

    @Test
    fun `rejects empty files and non ELF content`() {
        val empty = File.createTempFile("empty", ".so")
        val html = File.createTempFile("error", ".so")
        try {
            assertFalse(isValidDriverLibrary(empty))
            html.writeText("<html>Not Found</html>")
            assertFalse(isValidDriverLibrary(html))
            assertFalse(isValidDriverLibrary(File(html.parent, "missing.so")))
        } finally {
            empty.delete()
            html.delete()
        }
    }
    @Test
    fun `rejects archive names that escape the driver directory`() {
        val root = File(System.getProperty("java.io.tmpdir"), "driver-validation-root")
        assertTrue(runCatching { driverInstallDirectory(root, "Turnip.zip") }.isSuccess)
        assertTrue(runCatching { driverInstallDirectory(root, "../../victim.zip") }.isFailure)
        assertTrue(runCatching { driverInstallDirectory(root, "..\\victim.zip") }.isFailure)
        assertTrue(runCatching { driverInstallDirectory(root, ".zip") }.isFailure)
    }

    @Test
    fun `failed replacement preserves the installed driver`() {
        val root = kotlin.io.path.createTempDirectory("driver-replace").toFile()
        try {
            val target = File(root, "driver").apply { mkdirs() }
            File(target, "old.so").writeText("old")
            val missingStaging = File(root, "missing-staging")
            assertTrue(runCatching { replaceInstalledDriver(missingStaging, target) }.isFailure)
            assertTrue(File(target, "old.so").isFile)
        } finally { root.deleteRecursively() }
    }

    @Test
    fun `custom driver loading requires Android 9 or newer`() {
        assertFalse(customDriverSupportedOnApi(26))
        assertFalse(customDriverSupportedOnApi(27))
        assertTrue(customDriverSupportedOnApi(28))
    }

}

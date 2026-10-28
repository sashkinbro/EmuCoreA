package com.sbro.emucorea.core

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class CustomVulkanDriverDeviceTest {
    @Test
    fun loadsCompatibleDriverIntoVulkan() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val archive = File(context.cacheDir, "emucorea-qa-driver.zip")
        assumeTrue("Supply the compatible test archive in the app cache", archive.isFile)
        assumeTrue("Device does not support custom Vulkan drivers", GpuDriverCompatibility.supportsCustomDrivers())
        val manager = GpuDriverManager(context)
        val name = "emucorea-qa-driver"
        check(manager.readMainLibraryPath(name) == null) { "Test driver already exists" }
        try {
            val installed = manager.installFromArchive(archive, GpuDriverVendor.ADRENO)
            val path = requireNotNull(manager.readMainLibraryPath(installed))
            CoreRuntime.initialize(context)
            NativePpsspp.nativeSetCustomDriver(path, context.applicationInfo.nativeLibraryDir)
            assertTrue("The custom Vulkan driver did not load", NativePpsspp.nativeIsCustomDriverLoaded())
        } finally {
            NativePpsspp.nativeSetCustomDriver("", context.applicationInfo.nativeLibraryDir)
            manager.remove(name)
            archive.delete()
        }
    }
}

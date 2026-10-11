package com.sbro.emucorea.core

import java.util.Locale
import org.junit.Assert.*
import org.junit.Test

class PspNetworkPolicyTest {
    @Test
    fun androidDefaultPortOffsetMatchesPpssppPeers() {
        assertEquals("10000", PpssppCoreOptions.option("ppsspp_port_offset")!!.defaultValue)
    }

    @Test
    fun defaultUpnpMapsTheOffsetPortAsInPpsspp() {
        assertEquals("disabled", PpssppCoreOptions.option("ppsspp_upnp_use_original_port")!!.defaultValue)
    }

    @Test
    fun generatedIdentitiesAreDistinctAndMatchPpssppGameCompatibilityBits() {
        val addresses = (1..128).map { newPspNetworkIdentity() }
        assertEquals(addresses.size, addresses.toSet().size)
        addresses.forEach {
            assertTrue(validPspMac(it))
            assertEquals(0, it.take(2).toInt(16) and 3)
        }
        assertFalse(validPspMac("000000000000"))
        assertFalse(validPspMac("ffffffffffff"))
        assertFalse(validPspMac("010203040506"))
        assertFalse(validPspMac("invalid"))
    }

    @Test
    fun identityKeysStayAsciiUnderArabicLocale() {
        val previous = Locale.getDefault()
        try {
            Locale.setDefault(Locale.forLanguageTag("ar"))
            val address = newPspNetworkIdentity()
            assertTrue(address.matches(Regex("[0-9a-f]{12}")))
            assertTrue(networkMacKeys.all { it.matches(Regex("ppsspp_change_mac_address[0-9]{2}")) })
        } finally {
            Locale.setDefault(previous)
        }
    }

    @Test
    fun activeNetworkEditIsStagedWithoutCallingJniAndSurvivesNextBoot() {
        val activeField = CoreRuntime::class.java.getDeclaredField("sessionActive").apply { isAccessible = true }
        val optionsField = CoreRuntime::class.java.getDeclaredField("sessionCoreOptions").apply { isAccessible = true }
        @Suppress("UNCHECKED_CAST")
        val options = optionsField.get(CoreRuntime) as MutableMap<String, String>
        val previousActive = activeField.getBoolean(CoreRuntime)
        val previousOptions = options.toMap()
        try {
            activeField.setBoolean(CoreRuntime, true)
            CoreRuntime.applyCoreOption("ppsspp_port_offset", "20000")
            assertEquals("20000", options["ppsspp_port_offset"])
            val boot = runtimeStartCoreOptions(mapOf("ppsspp_port_offset" to "10000"),
                emptyMap(), options, "Auto", null)
            assertEquals("20000", boot["ppsspp_port_offset"])
        } finally {
            options.clear()
            options.putAll(previousOptions)
            activeField.setBoolean(CoreRuntime, previousActive)
        }
    }
}

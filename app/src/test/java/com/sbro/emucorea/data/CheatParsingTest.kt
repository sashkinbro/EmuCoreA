package com.sbro.emucorea.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CheatParsingTest {

    @Test
    fun parsesPpssppIniForMatchingSerial() {
        val raw = """
            _S ULUS-10041
            _G Test Game
            _C0 Infinite Health
            _L 0x01234567 0x63
            _C1 Max Money
            _L 0x00AABBCC 0xFFFFFFFF
        """.trimIndent()

        val blocks = parseCheatBlocks(raw, "ULUS10041")

        assertEquals(2, blocks.size)
        assertEquals("Infinite Health", blocks[0].title)
        assertFalse(blocks[0].enabled)
        assertEquals(listOf("01234567 00000063"), blocks[0].lines)
        assertEquals("Max Money", blocks[1].title)
        assertTrue(blocks[1].enabled)
        assertEquals(listOf("00AABBCC FFFFFFFF"), blocks[1].lines)
    }

    @Test
    fun filtersOutSectionsForOtherGames() {
        val raw = """
            _S ULUS10041
            _C1 First Game Cheat
            _L 0x00000001 0x00000002
            _S ULES00001
            _C1 Second Game Cheat
            _L 0x00000003 0x00000004
        """.trimIndent()

        val blocks = parseCheatBlocks(raw, "ULES00001")

        assertEquals(1, blocks.size)
        assertEquals("Second Game Cheat", blocks[0].title)
        assertEquals(listOf("00000003 00000004"), blocks[0].lines)
    }

    @Test
    fun keepsEverySectionWhenNoSerialIsKnown() {
        val raw = """
            _S ULUS10041
            _C1 First
            _L 0x1 0x2
            _S ULES00001
            _C1 Second
            _L 0x3 0x4
        """.trimIndent()

        val blocks = parseCheatBlocks(raw)

        assertEquals(2, blocks.size)
    }

    @Test
    fun dropsBlocksOutsideAnySectionWhenFiltering() {
        val raw = """
            _C1 Orphan
            _L 0x1 0x2
            _S ULUS10041
            _C1 Real
            _L 0x3 0x4
        """.trimIndent()

        val blocks = parseCheatBlocks(raw, "ULUS10041")

        assertEquals(1, blocks.size)
        assertEquals("Real", blocks[0].title)
    }

    @Test
    fun stripsBomAndNormalizesPpssppCodeLines() {
        val raw = "\uFEFF_S UCES01234\n_C1 Lowercase\n_L 8000 abcd\n"

        val blocks = parseCheatBlocks(raw, "UCES01234")

        assertEquals(listOf("00008000 0000ABCD"), blocks.single().lines)
    }

    @Test
    fun ignoresUnsupportedPpssppLinesButKeepsValidOnes() {
        val raw = """
            _S ULUS10041
            _C1 Mixed
            _M 0x00000000 0x00000000
            _L not a code
            _L 0x0000FFFF 0x00000001
            _L 0x00000000 0x123456789
        """.trimIndent()

        val blocks = parseCheatBlocks(raw, "ULUS10041")

        assertEquals(1, blocks.size)
        assertEquals(listOf("0000FFFF 00000001"), blocks[0].lines)
    }

    @Test
    fun rawCodePairsStillParse() {
        val raw = "// Infinite HP\n01234567 00000001\n89ABCDEF 00000002\n"

        val blocks = parseCheatBlocks(raw)

        assertEquals(1, blocks.size)
        assertEquals("Infinite HP", blocks[0].title)
        assertEquals(listOf("01234567 00000001", "89ABCDEF 00000002"), blocks[0].lines)
        assertFalse(blocks[0].enabled)
    }

    @Test
    fun libretroFormatStillParses() {
        val raw = """
            cheats = 1
            cheat0_desc = "Infinite HP"
            cheat0_code = "01234567+00000001+89ABCDEF+00000002"
            cheat0_enable = false
        """.trimIndent()

        val blocks = parseCheatBlocks(raw)

        assertEquals(1, blocks.size)
        assertEquals("Infinite HP", blocks[0].title)
        assertEquals(listOf("01234567 00000001", "89ABCDEF 00000002"), blocks[0].lines)
    }

    @Test
    fun ps2PatchLinesStillParse() {
        val raw = "// Some patch\npatch=1,EE,00123456,word,00000001\n"

        val blocks = parseCheatBlocks(raw)

        assertEquals(1, blocks.size)
        assertEquals("Some patch", blocks[0].title)
        assertEquals(listOf("patch=1,EE,00123456,word,00000001"), blocks[0].lines)
    }
}

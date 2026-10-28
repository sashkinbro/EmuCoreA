package com.sbro.emucorea.core

import org.junit.Assert.*
import org.junit.Test

class ArchiveGamePathTest {
    @Test fun archivePathsRoundTripContentUrisAndUnicodeEntryNames() {
        val source = "content://provider/tree/primary%3AGames/document/primary%3AGames%2Fa.7z"
        val entry = "Каталог/гра #1.ISO"
        val path = ArchiveGamePath.create(source, entry, 3)
        assertEquals(ArchiveGamePath.Entry(source, entry, 3), ArchiveGamePath.parse(path))
        assertNull(ArchiveGamePath.parse(source))
        assertNull(ArchiveGamePath.parse("archivegame:invalid"))
    }

    @Test fun legacyZipIdentityFollowsTheOriginalDiscSelection() {
        assertEquals(2, ArchiveGamePath.legacyZipMemberIndex(listOf("tool.elf", "deep/folder/other.iso", "disc/game.cso", "another.iso")))
        assertEquals(-1, ArchiveGamePath.legacyZipMemberIndex(listOf("__MACOSX/game.iso", "tool.elf")))
    }

    @Test fun recognizesOnlyPlayableMembersAndNotNestedArchives() {
        for (name in listOf("folder/game.ISO", "EBOOT.PBP", "test.chd", "BOOT.BIN")) {
            assertTrue(name, ArchiveGamePath.isPlayableMember(name))
        }
        for (name in listOf("folder/", "game.zip", "game.7z", "game.rar", "track.bin", "cover.png", "__MACOSX/game.iso")) {
            assertFalse(name, ArchiveGamePath.isPlayableMember(name))
        }
    }
}

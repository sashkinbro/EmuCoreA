package com.sbro.emucorea.core

import org.junit.Assert.*
import org.junit.Test

class PspGameFormatsTest {
    @Test fun acceptsCoreExecutableAndDiscNamesRegardlessOfCase() {
        for (name in listOf("game.ISO", "game.cso", "game.CHD", "EBOOT.PBP", "cube.ELF", "cube.prx", "cube.plf", "game.ZIP", "game.7z", "game.RAR", "BOOT.BIN", "EBOOT.BIN")) {
            assertTrue(name, PspGameFormats.isSupportedName(name))
        }
    }

    @Test fun rejectsTrackFilesArchivesAndUnrelatedFiles() {
        for (name in listOf("track.bin", "game.cue", "game.zso", "game.pkg", "save.ppst", "frame.ppdmp", "game.iso.png", "")) {
            assertFalse(name, PspGameFormats.isSupportedName(name))
        }
    }
}

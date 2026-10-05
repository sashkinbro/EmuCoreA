package com.sbro.emucorea.data

import org.junit.Assert.assertEquals
import org.junit.Test

class TexturePackLayoutTest {
    @Test fun nestedAssetFoldersArePreserved() {
        val paths = setOf("pack-main/PSP/TEXTURES/ULES00151/textures.ini",
            "pack-main/PSP/TEXTURES/ULES00151/textures/ui/icon.png",
            "pack-main/PSP/TEXTURES/ULES00151/regions/eu.ini", "outside.png")
        assertEquals(listOf("textures.ini", "textures/ui/icon.png", "regions/eu.ini"),
            resolveTexturePackSections(paths).single().files.values.toList())
    }
    @Test fun flatPackKeepsRelativePaths() {
        assertEquals(mapOf("textures.ini" to "textures.ini", "replacements/a.png" to "replacements/a.png"),
            resolveTexturePackSections(setOf("textures.ini", "replacements/a.png")).single().files)
    }
    @Test fun mainIniIsCanonicalized() {
        assertEquals(mapOf("Pack/Textures.INI" to "textures.ini", "Pack/A.png" to "A.png"),
            resolveTexturePackSections(setOf("Pack/Textures.INI", "Pack/A.png")).single().files)
    }
    @Test fun multiRootPacksKeepOneSectionPerIni() {
        val sections = resolveTexturePackSections(
            setOf("A/textures.ini", "A/a.png", "B/textures.ini", "B/b.png"))
        assertEquals(listOf("A/textures.ini", "B/textures.ini"), sections.map { it.iniPath })
        assertEquals(mapOf("A/textures.ini" to "textures.ini", "A/a.png" to "a.png"), sections[0].files)
        assertEquals(mapOf("B/textures.ini" to "textures.ini", "B/b.png" to "b.png"), sections[1].files)
    }
    @Test(expected = IllegalStateException::class) fun missingIniIsRejected() {
        resolveTexturePackSections(setOf("A.png"))
    }
}

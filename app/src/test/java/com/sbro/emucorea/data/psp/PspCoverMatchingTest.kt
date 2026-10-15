package com.sbro.emucorea.data.psp

import org.junit.Assert.*
import org.junit.Test
import org.json.JSONObject
import java.io.File

class PspCoverMatchingTest {
    @Test fun bundledIdentityMapsAreConsistentWithRuntimeNormalization() {
        val index = JSONObject(File("src/main/assets/catalog/psp_covers.json").readText())
        val games = index.getJSONObject("games")
        for (mapName in listOf("serials", "titles")) {
            val map = index.getJSONObject(mapName)
            map.keys().forEach { key ->
                assertTrue("Dangling $mapName key: $key", games.has(map.getString(key)))
                if (mapName == "titles") assertEquals(key, PspCoverIndexRepository.normalizeTitle(key))
                else assertEquals(key, PspCoverIndexRepository.normalizeSerial(key))
            }
        }
        assertEquals("195868", index.getJSONObject("titles").getString("tekken 6"))
        assertEquals("16665", index.getJSONObject("serials").getString("ULJM08035"))
        val corrections = mapOf("UCJS10114" to "550", "ULJM05438" to "552", "ULES01438" to "142710", "UCJS10102" to "21665", "ULES00806" to "44506", "ULES01333" to "4749", "ULUS10428" to "4749")
        corrections.forEach { (serial, id) -> assertEquals(serial, id, index.getJSONObject("serials").getString(serial)) }
    }

    @Test fun trademarksDoNotBecomeTitleWords() {
        assertEquals("god of war chains of olympus", PspCoverIndexRepository.normalizeTitle("God of War™: Chains of Olympus®"))
    }

    @Test fun filenamesKeepTheTitleButLoseDiscMetadata() {
        assertEquals("wwe smackdown vs raw 2011", PspCoverIndexRepository.normalizeTitle("WWE SmackDown vs. Raw 2011 (Europe) (En,Fr,De).iso"))
        assertEquals("sora no kiseki sc", PspCoverIndexRepository.normalizeTitle("Sora no Kiseki SC (Disc 2)"))
        assertEquals("game subtitle", PspCoverIndexRepository.normalizeTitle("Game (Subtitle)"))
        assertEquals("game subtitle", normalizeIdentityTitle("Game (Subtitle)"))
    }

    @Test fun sequelsAndSportsSeasonsCannotFuzzyMatchEachOther() {
        assertEquals(0.0, catalogTitleSimilarity("God of War", "God of War II"), 0.0)
        assertEquals(0.0, catalogTitleSimilarity("FIFA Soccer 12", "FIFA Soccer 13"), 0.0)
        assertEquals(0.0, catalogTitleSimilarity("Final Fantasy IV", "Final Fantasy VI"), 0.0)
        assertEquals(1.0, catalogTitleSimilarity("Final Fantasy IV", "Final Fantasy 4"), 0.0)
    }
}

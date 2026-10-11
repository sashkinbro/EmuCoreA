package com.sbro.emucorea.data

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DlcMetadataTest {

    @Test
    fun `detects dlc by param sfo category`() {
        assertTrue(GameRepository.isDlcMetadata(category = "AC", title = "Any Game"))
        assertTrue(GameRepository.isDlcMetadata(category = "ac", title = "Any Game"))
        assertFalse(GameRepository.isDlcMetadata(category = "UG", title = "Any Game"))
        assertFalse(GameRepository.isDlcMetadata(category = "DG", title = "Any Game"))
    }

    @Test
    fun `detects dlc by localized downloadable content titles`() {
        assertTrue(GameRepository.isDlcMetadata(null, "Some Game downloadable content"))
        assertTrue(GameRepository.isDlcMetadata(null, "Some Game загружаемый контент"))
        assertTrue(GameRepository.isDlcMetadata(null, "Some Game 追加コンテンツ"))
    }

    @Test
    fun `detects dlc by standalone filename marker`() {
        assertTrue(GameRepository.isDlcMetadata(null, "Burnout Legends", "Burnout Legends DLC.cso"))
        assertTrue(GameRepository.isDlcMetadata(null, "Wipeout Pure", "Wipeout Pure [DLC].cso"))
        assertTrue(GameRepository.isDlcMetadata(null, "Wipeout Pure", "Wipeout Pure (DLC Pack).cso"))
        assertTrue(GameRepository.isDlcMetadata(null, "Ridge Racer", "ridge racer - dlc - us.pbp"))
        assertTrue(GameRepository.isDlcMetadata(null, null, "DLC.cso"))
    }

    @Test
    fun `does not match dlc inside ordinary words or clean game names`() {
        assertFalse(GameRepository.isDlcMetadata(null, "Padlock Crisis"))
        assertFalse(GameRepository.isDlcMetadata(null, "Metal Gear Solid", "Metal Gear Solid.cso"))
        assertFalse(GameRepository.isDlcMetadata(null, "Daxter", "Daxter.cso"))
        assertFalse(GameRepository.isDlcMetadata(null, null, null))
    }

    @Test
    fun `title alone still triggers detection`() {
        assertTrue(GameRepository.isDlcMetadata(null, "Patapon DLC Pack", "patapon.cso"))
    }
}

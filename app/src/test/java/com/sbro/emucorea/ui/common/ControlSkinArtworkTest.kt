package com.sbro.emucorea.ui.common

import android.graphics.Bitmap
import android.graphics.Color
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = android.app.Application::class)
class ControlSkinArtworkTest {
    @Test
    fun trimsTransparentPaddingWithoutStretchingArtwork() {
        val artwork = Bitmap.createBitmap(12, 12, Bitmap.Config.ARGB_8888)
        artwork.setPixel(2, 5, Color.WHITE)
        artwork.setPixel(9, 8, Color.GREEN)

        val (fitted, pressed) = fitSkinArtwork(artwork, null)

        assertEquals(8, fitted.width)
        assertEquals(4, fitted.height)
        assertEquals(Color.WHITE, fitted.getPixel(0, 0))
        assertEquals(Color.GREEN, fitted.getPixel(7, 3))
        assertNull(pressed)
    }

    @Test
    fun pressedArtworkUsesTheSameCropAsNormalArtwork() {
        val normal = Bitmap.createBitmap(12, 12, Bitmap.Config.ARGB_8888)
        val pressed = Bitmap.createBitmap(12, 12, Bitmap.Config.ARGB_8888)
        normal.setPixel(2, 2, Color.WHITE)
        normal.setPixel(9, 9, Color.WHITE)
        pressed.setPixel(5, 5, Color.GREEN)

        val (fitted, fittedPressed) = fitSkinArtwork(normal, pressed)
        requireNotNull(fittedPressed)

        assertEquals(fitted.width, fittedPressed.width)
        assertEquals(fitted.height, fittedPressed.height)
        assertEquals(Color.TRANSPARENT, fittedPressed.getPixel(0, 0))
        assertEquals(Color.GREEN, fittedPressed.getPixel(3, 3))
    }

    @Test
    fun keepsFullyTransparentArtwork() {
        val empty = Bitmap.createBitmap(12, 12, Bitmap.Config.ARGB_8888)
        assertSame(empty, fitSkinArtwork(empty, null).first)
    }
}

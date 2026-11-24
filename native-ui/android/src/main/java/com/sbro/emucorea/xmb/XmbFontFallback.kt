// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

package com.sbro.emucorea.xmb

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.Typeface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentHashMap
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.roundToInt

/**
 * Rasterizes Unicode codepoints absent from `ltn0.pgf` (Cyrillic, CJK, Arabic,
 * Polish/Czech diacritics, Devanagari, etc.) using Android's system sans font,
 * matching PGF pixel metrics (`textSize = 20f` -> cap height ~14-15px at scale 1).
 */
object XmbFontFallback {
    private const val MAX_GLYPH_DIM = 128
    private val lock = Any()
    private val glyphCache = ConcurrentHashMap<Int, ByteArray>()
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.DEFAULT
        textSize = 20f
        color = Color.WHITE
    }
    private val bounds = Rect()
    private val sharedBitmap = Bitmap.createBitmap(MAX_GLYPH_DIM, MAX_GLYPH_DIM, Bitmap.Config.ARGB_8888)
    private val sharedCanvas = Canvas(sharedBitmap)
    private val sharedPixels = IntArray(MAX_GLYPH_DIM * MAX_GLYPH_DIM)

    /**
     * Returns `[width, height, left, top, advanceH]` (little-endian int32 x 5 = 20 bytes)
     * followed by `width * height` unsigned alpha bytes (`0..255`), or `null` if invalid.
     */
    @JvmStatic
    fun rasterizeGlyph(codepoint: Int): ByteArray? {
        if (codepoint <= 0 || !Character.isValidCodePoint(codepoint)) {
            return null
        }
        glyphCache[codepoint]?.let { return it }

        val text = String(Character.toChars(codepoint))
        synchronized(lock) {
            glyphCache[codepoint]?.let { return it }

            val advancePx = paint.measureText(text)
            val advanceH = (advancePx * 64f).roundToInt()
            if (Character.isWhitespace(codepoint)) {
                val out = ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN)
                out.putInt(0)
                out.putInt(0)
                out.putInt(0)
                out.putInt(0)
                out.putInt(advanceH)
                val bytes = out.array()
                glyphCache[codepoint] = bytes
                return bytes
            }

            paint.getTextBounds(text, 0, text.length, bounds)
            val pad = 1
            val left = floor(bounds.left.toDouble()).toInt() - pad
            val topY = floor(bounds.top.toDouble()).toInt() - pad
            val right = ceil(bounds.right.toDouble()).toInt() + pad
            val bottom = ceil(bounds.bottom.toDouble()).toInt() + pad
            val w = (right - left).coerceIn(1, MAX_GLYPH_DIM)
            val h = (bottom - topY).coerceIn(1, MAX_GLYPH_DIM)
            val topFromBaseline = -topY

            sharedBitmap.eraseColor(Color.TRANSPARENT)
            sharedCanvas.drawText(text, (-left).toFloat(), topFromBaseline.toFloat(), paint)
            sharedBitmap.getPixels(sharedPixels, 0, w, 0, 0, w, h)

            val pixelCount = w * h
            val out = ByteBuffer.allocate(20 + pixelCount).order(ByteOrder.LITTLE_ENDIAN)
            out.putInt(w)
            out.putInt(h)
            out.putInt(left)
            out.putInt(topFromBaseline)
            out.putInt(advanceH)
            for (i in 0 until pixelCount) {
                out.put(((sharedPixels[i] ushr 24) and 0xFF).toByte())
            }
            val bytes = out.array()
            glyphCache[codepoint] = bytes
            return bytes
        }
    }
}

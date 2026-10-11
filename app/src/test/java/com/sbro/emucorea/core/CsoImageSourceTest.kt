package com.sbro.emucorea.core

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Random
import java.util.zip.Deflater

class CsoImageSourceTest {

    private companion object {
        const val PLAIN_FLAG = 1 shl 31
        const val OFFSET_MASK = 0x7FFFFFFF
    }

    private class MemorySource(private val bytes: ByteArray) : PspImageSource {
        override val size: Long
            get() = bytes.size.toLong()

        override fun read(offset: Long, length: Int): ByteArray? {
            if (length <= 0 || offset < 0 || offset + length > bytes.size) return null
            return bytes.copyOfRange(offset.toInt(), (offset + length).toInt())
        }

        override fun close() = Unit
    }

    /** Builds a CISO container following the real on-disk layout (see PPSSPP BlockDevices.cpp). */
    private fun buildCso(
        iso: ByteArray,
        blockSize: Int,
        align: Int,
        flushChain: Boolean,
        compress: (blockIndex: Int) -> Boolean
    ): ByteArray {
        val totalBlocks = (iso.size + blockSize - 1) / blockSize
        val header = ByteArray(24)
        header[0] = 'C'.code.toByte()
        header[1] = 'I'.code.toByte()
        header[2] = 'S'.code.toByte()
        header[3] = 'O'.code.toByte()
        header.writeIntLe(4, 24)
        writeLongLe(header, 8, iso.size.toLong())
        header.writeIntLe(16, blockSize)
        header[20] = 1 // version
        header[21] = align.toByte()
        val indexBytes = ByteArray((totalBlocks + 1) * 4)
        val data = ByteArrayOutputStream()
        // Index offsets are absolute file positions; the data section follows the index table.
        var offset = header.size + indexBytes.size
        var chainDeflater: Deflater? = null
        val buffer = ByteArray(64 * 1024)
        for (block in 0 until totalBlocks) {
            // High bit marks an UNCOMPRESSED frame in CISO; offsets are stored shifted right by align.
            val entry = ((offset shr align) and OFFSET_MASK) or if (compress(block)) 0 else PLAIN_FLAG
            indexBytes.writeIntLe(block * 4, entry)
            val start = block * blockSize
            val end = minOf(start + blockSize, iso.size)
            val plain = iso.copyOfRange(start, end)
            if (compress(block)) {
                val compressed = ByteArrayOutputStream()
                if (flushChain) {
                    if (chainDeflater == null) chainDeflater = Deflater(9, true)
                    chainDeflater.setInput(plain)
                    while (!chainDeflater.needsInput()) {
                        val count = chainDeflater.deflate(buffer, 0, buffer.size, Deflater.FULL_FLUSH)
                        compressed.write(buffer, 0, count)
                    }
                } else {
                    val deflater = Deflater(9, true)
                    deflater.setInput(plain)
                    deflater.finish()
                    while (!deflater.finished()) {
                        val count = deflater.deflate(buffer)
                        compressed.write(buffer, 0, count)
                    }
                    deflater.end()
                }
                data.write(compressed.toByteArray())
                data.write(0) // trailing byte reserved by the CISO layout
                offset += compressed.size() + 1
            } else {
                data.write(plain)
                offset += plain.size
            }
            if (align > 0) {
                val unit = 1 shl align
                val pad = ((unit - (offset % unit)) % unit)
                repeat(pad) { data.write(0) }
                offset += pad
            }
        }
        chainDeflater?.end()
        // The final index entry marks the end of the last block's storage.
        indexBytes.writeIntLe(totalBlocks * 4, (offset shr align) and OFFSET_MASK)
        return header + indexBytes + data.toByteArray()
    }

    private fun ByteArray.writeIntLe(offset: Int, value: Int) {
        ByteBuffer.wrap(this, offset, 4).order(ByteOrder.LITTLE_ENDIAN).putInt(value)
    }

    private fun writeLongLe(target: ByteArray, offset: Int, value: Long) {
        for (i in 0 until 8) target[offset + i] = ((value shr (8 * i)) and 0xFF).toByte()
    }

    private fun sampleIso(blocks: Int, tail: Int): ByteArray {
        val random = Random(42)
        val iso = ByteArray(blocks * 2048 + tail)
        random.nextBytes(iso)
        return iso
    }

    @Test
    fun `reads mixed compressed and plain blocks exactly like the raw iso`() {
        val iso = sampleIso(blocks = 5, tail = 777)
        val cso = buildCso(iso, blockSize = 2048, align = 0, flushChain = false) { it % 2 == 0 }
        val source = CsoImageSource.open(MemorySource(cso))

        assertNotNull(source)
        assertArrayEquals(iso, source!!.read(0, iso.size))
        assertFalse(source.isPbp())
        source.close()
    }

    @Test
    fun `reads flush chained unterminated block streams`() {
        val iso = sampleIso(blocks = 4, tail = 0)
        val cso = buildCso(iso, blockSize = 2048, align = 0, flushChain = true) { true }
        val source = CsoImageSource.open(MemorySource(cso))!!

        assertArrayEquals(iso, source.read(0, iso.size))
        source.close()
    }

    @Test
    fun `reads slices spanning block boundaries and the short final block`() {
        val iso = sampleIso(blocks = 6, tail = 13)
        val cso = buildCso(iso, blockSize = 2048, align = 0, flushChain = false) { true }
        val source = CsoImageSource.open(MemorySource(cso))!!

        listOf(
            0L to 1,
            2047L to 2, // boundary span
            2048L to 2048,
            4096L to 5000, // multi-block read
            (iso.size - 13).toLong() to 13,
            (iso.size - 1).toLong() to 1
        ).forEach { (offset, length) ->
            assertArrayEquals(
                "offset=$offset length=$length",
                iso.copyOfRange(offset.toInt(), (offset + length).toInt()),
                source.read(offset, length)!!
            )
        }
        source.close()
    }

    @Test
    fun `rejects reads past the end and non ciso containers`() {
        val iso = sampleIso(blocks = 2, tail = 0)
        val cso = buildCso(iso, blockSize = 2048, align = 0, flushChain = false) { true }
        val source = CsoImageSource.open(MemorySource(cso))!!

        assertNull(source.read(iso.size.toLong(), 4))
        assertNull(source.read(0, iso.size + 1))
        assertNull(source.read(-1, 4))
        source.close()

        assertNull(CsoImageSource.open(MemorySource(iso)))
    }

    @Test
    fun `supports aligned indexes`() {
        val iso = sampleIso(blocks = 4, tail = 100)
        val cso = buildCso(iso, blockSize = 2048, align = 2, flushChain = false) { true }
        val source = CsoImageSource.open(MemorySource(cso))!!

        assertArrayEquals(iso, source.read(0, iso.size))
        source.close()
    }

    @Test
    fun `reads blocks written without the reserved trailing byte`() {
        // A writer that packs real stream data into the final byte: the trim retry
        // in inflateBlock must still reconstruct the block.
        val iso = sampleIso(blocks = 3, tail = 0)
        val totalBlocks = iso.size / 2048
        val header = ByteArray(24)
        header[0] = 'C'.code.toByte()
        header[1] = 'I'.code.toByte()
        header[2] = 'S'.code.toByte()
        header[3] = 'O'.code.toByte()
        header.writeIntLe(4, 24)
        writeLongLe(header, 8, iso.size.toLong())
        header.writeIntLe(16, 2048)
        header[20] = 1
        val indexBytes = ByteArray((totalBlocks + 1) * 4)
        val data = ByteArrayOutputStream()
        var offset = header.size + indexBytes.size
        for (block in 0 until totalBlocks) {
            indexBytes.writeIntLe(block * 4, offset)
            val plain = iso.copyOfRange(block * 2048, (block + 1) * 2048)
            val deflater = Deflater(9, true)
            deflater.setInput(plain)
            deflater.finish()
            val buffer = ByteArray(4096)
            val compressed = ByteArrayOutputStream()
            while (!deflater.finished()) {
                val count = deflater.deflate(buffer)
                compressed.write(buffer, 0, count)
            }
            deflater.end()
            // No reserved trailing byte: the span ends exactly at the stream.
            data.write(compressed.toByteArray())
            offset += compressed.size()
        }
        indexBytes.writeIntLe(totalBlocks * 4, offset and OFFSET_MASK)
        val cso = header + indexBytes + data.toByteArray()
        val source = CsoImageSource.open(MemorySource(cso))!!

        assertArrayEquals(iso, source.read(0, iso.size))
        source.close()
    }
}

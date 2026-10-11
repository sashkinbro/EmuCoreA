package com.sbro.emucorea.core

import java.io.Closeable
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.LinkedHashMap
import java.util.zip.DataFormatException
import java.util.zip.Inflater

internal const val PSP_ASSET_PARAM_SFO = 0
internal const val PSP_ASSET_ICON0 = 1

private const val ISO_SECTOR_BYTES = 2048
private const val ISO_PRIMARY_VOLUME_DESCRIPTOR_SECTOR = 16
private const val ISO_ROOT_RECORD_OFFSET = 156
private const val ISO_DIRECTORY_ENTRY_BYTES = 34
private const val MAX_ASSET_BYTES = 4 * 1024 * 1024
private const val MAX_DIRECTORY_BYTES = 512 * 1024

/** Random-access view over the logical ISO9660 bytes of a PSP game image. */
internal interface PspImageSource : Closeable {
    val size: Long

    /** Returns the full requested range or null; partial reads are never returned. */
    fun read(offset: Long, length: Int): ByteArray?

    fun isPbp(): Boolean {
        val magic = read(0, 4) ?: return false
        return magic[0] == 0.toByte() && magic[1] == 'P'.code.toByte() &&
            magic[2] == 'B'.code.toByte() && magic[3] == 'P'.code.toByte()
    }

    fun readPbpAsset(asset: Int): ByteArray? {
        val header = read(0, 0x28) ?: return null
        val offsets = IntArray(8) { littleInt(header, 8 + it * 4) }
        val start = offsets.getOrNull(asset)?.toLong() ?: return null
        val end = offsets.getOrNull(asset + 1)?.toLong() ?: size
        if (start <= 0L || end <= start) return null
        val length = (end - start).coerceAtMost(MAX_ASSET_BYTES.toLong()).toInt()
        return read(start, length)
    }

    fun readIsoAsset(asset: Int): ByteArray? {
        val name = if (asset == PSP_ASSET_PARAM_SFO) "PARAM.SFO" else "ICON0.PNG"
        val descriptor = read(
            ISO_PRIMARY_VOLUME_DESCRIPTOR_SECTOR.toLong() * ISO_SECTOR_BYTES,
            ISO_SECTOR_BYTES
        ) ?: return null
        if (descriptor.size <= ISO_ROOT_RECORD_OFFSET || descriptor[0].toInt() != 1) return null
        val root = parseDirectoryRecord(descriptor, ISO_ROOT_RECORD_OFFSET) ?: return null
        val gameDirectory = findEntry(readDirectory(this, root), "PSP_GAME") ?: return null
        if (!gameDirectory.isDirectory) return null
        val entry = findEntry(readDirectory(this, gameDirectory), name) ?: return null
        if (entry.isDirectory || entry.length <= 0L || entry.length > MAX_ASSET_BYTES) return null
        return read(entry.extent * ISO_SECTOR_BYTES, entry.length.toInt())
    }
}

private data class IsoEntry(
    val name: String,
    val extent: Long,
    val length: Long,
    val isDirectory: Boolean
)

private fun readDirectory(source: PspImageSource, directory: IsoEntry): List<IsoEntry> {
    val length = directory.length.coerceAtMost(MAX_DIRECTORY_BYTES.toLong()).toInt()
    val data = source.read(directory.extent * ISO_SECTOR_BYTES, length) ?: return emptyList()
    val entries = mutableListOf<IsoEntry>()
    var position = 0
    while (position + ISO_DIRECTORY_ENTRY_BYTES <= data.size) {
        val recordLength = data[position].toInt() and 0xFF
        if (recordLength == 0) {
            // A zero length moves to the next logical sector.
            val next = ((position / ISO_SECTOR_BYTES) + 1) * ISO_SECTOR_BYTES
            if (next <= position) break
            position = next
            continue
        }
        if (position + recordLength > data.size) break
        parseDirectoryRecord(data, position)?.let(entries::add)
        position += recordLength
    }
    return entries
}

private fun parseDirectoryRecord(buffer: ByteArray, offset: Int): IsoEntry? {
    if (offset + ISO_DIRECTORY_ENTRY_BYTES > buffer.size) return null
    val recordLength = buffer[offset].toInt() and 0xFF
    if (recordLength < ISO_DIRECTORY_ENTRY_BYTES || offset + recordLength > buffer.size) return null
    val extent = littleInt(buffer, offset + 2).toLong() and 0xFFFFFFFFL
    val length = littleInt(buffer, offset + 10).toLong() and 0xFFFFFFFFL
    val flags = buffer[offset + 25].toInt() and 0xFF
    val nameLength = buffer[offset + 32].toInt() and 0xFF
    if (nameLength <= 0 || offset + 33 + nameLength > buffer.size) return null
    val rawName = String(buffer, offset + 33, nameLength, Charsets.US_ASCII)
    // Root records use a single NUL; files carry a ";1" version suffix.
    val name = rawName.trimEnd('\u0000').substringBefore(';')
    return IsoEntry(
        name = name,
        extent = extent,
        length = length,
        isDirectory = flags and 0x02 != 0
    )
}

private fun findEntry(entries: List<IsoEntry>, name: String): IsoEntry? =
    entries.firstOrNull { it.name.equals(name, ignoreCase = true) }

private fun littleInt(bytes: ByteArray, offset: Int): Int =
    ByteBuffer.wrap(bytes, offset, 4).order(ByteOrder.LITTLE_ENDIAN).int

/**
 * Reads a CSO container as logical ISO9660 bytes, inflating blocks on demand.
 *
 * Compressed blocks use raw deflate; each stream is independently inflatable
 * because CISO compressors emit a full flush after every block. ZSO (zstd) is
 * intentionally not supported here and falls back to the raw source.
 */
internal class CsoImageSource private constructor(
    private val raw: PspImageSource,
    override val size: Long,
    private val blockSize: Int,
    private val align: Int,
    private val index: IntArray
) : PspImageSource {

    private val cache = object : LinkedHashMap<Int, ByteArray>(16, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<Int, ByteArray>?): Boolean =
            size > CACHE_LIMIT
    }

    override fun isPbp(): Boolean = false

    override fun read(offset: Long, length: Int): ByteArray? {
        if (length <= 0 || offset < 0 || offset + length > size) return null
        val out = ByteArray(length)
        var filled = 0
        while (filled < length) {
            val position = offset + filled
            val block = block((position / blockSize).toInt()) ?: return null
            val inBlock = (position % blockSize).toInt()
            val toCopy = minOf(block.size - inBlock, length - filled)
            System.arraycopy(block, inBlock, out, filled, toCopy)
            filled += toCopy
        }
        return out
    }

    override fun close() {
        raw.close()
    }

    private fun block(blockIndex: Int): ByteArray? {
        synchronized(cache) { cache[blockIndex] }?.let { return it }
        if (blockIndex < 0 || blockIndex >= totalBlocks) return null
        val entry = index[blockIndex]
        val start = (entry and OFFSET_MASK).toLong() shl align
        val end = ((index[blockIndex + 1] and OFFSET_MASK).toLong() shl align).coerceAtLeast(start + 1)
        val rawBytes = raw.read(start, (end - start).toInt()) ?: return null
        val expected = if (blockIndex == totalBlocks - 1) {
            (size - blockIndex.toLong() * blockSize).toInt().coerceIn(1, blockSize)
        } else {
            blockSize
        }
        // In CISO the high index bit marks an UNCOMPRESSED (plain) frame.
        val block = if (entry and PLAIN_FLAG != 0) {
            rawBytes.copyOf(expected)
        } else {
            inflateBlock(rawBytes, expected) ?: return null
        }
        synchronized(cache) { cache[blockIndex] = block }
        return block
    }

    private fun inflateBlock(data: ByteArray, expected: Int): ByteArray? {
        // Each frame ends before CISO's reserved trailing byte, so the full span
        // normally inflates on its own; the trim retry covers nonconforming writers.
        for (trim in intArrayOf(0, 1)) {
            val available = data.size - trim
            if (available <= 0) continue
            val inflater = Inflater(true)
            try {
                inflater.setInput(data, 0, available)
                val out = ByteArray(expected)
                var produced = 0
                while (produced < expected) {
                    val count = inflater.inflate(out, produced, expected - produced)
                    if (count == 0) break
                    produced += count
                }
                if (produced == expected) return out
            } catch (_: DataFormatException) {
                // Try the next trim variant.
            } finally {
                inflater.end()
            }
        }
        return null
    }

    private val totalBlocks: Int
        get() = index.size - 1

    companion object {
        private const val HEADER_BYTES = 24
        private const val MAX_HEADER_BYTES = 64
        private const val PLAIN_FLAG = 1 shl 31
        private const val OFFSET_MASK = 0x7FFFFFFF
        private const val CACHE_LIMIT = 64
        private const val MAX_BLOCKS = 1 shl 20

        /** Returns null when the stream is not a CISO container; callers keep using the raw source. */
        fun open(raw: PspImageSource): CsoImageSource? = runCatching {
            val header = raw.read(0, HEADER_BYTES) ?: return null
            val isCiso = header[0] == 'C'.code.toByte() && header[1] == 'I'.code.toByte() &&
                header[2] == 'S'.code.toByte() && header[3] == 'O'.code.toByte()
            if (!isCiso) return null
            val headerSize = header.intAtLe(4)
            val size = ByteBuffer.wrap(header, 8, 8).order(ByteOrder.LITTLE_ENDIAN).long
            val blockSize = header.intAtLe(16)
            val align = header[21].toInt() and 0xFF
            if (size <= 0L || blockSize < ISO_SECTOR_BYTES || blockSize > MAX_FRAME_BYTES || align > 20) return null
            val totalBlocks = (((size + blockSize - 1) / blockSize).toInt())
            if (totalBlocks <= 0 || totalBlocks > MAX_BLOCKS) return null
            val indexAt = if (headerSize in HEADER_BYTES..MAX_HEADER_BYTES) headerSize else HEADER_BYTES
            val indexBytes = raw.read(indexAt.toLong(), (totalBlocks + 1) * 4) ?: return null
            val index = IntArray(totalBlocks + 1) { block ->
                ByteBuffer.wrap(indexBytes, block * 4, 4).order(ByteOrder.LITTLE_ENDIAN).int
            }
            CsoImageSource(raw, size, blockSize, align, index)
        }.getOrNull()

        private const val ISO_SECTOR_BYTES = 2048
        private const val MAX_FRAME_BYTES = 1024 * 1024

        private fun ByteArray.intAtLe(offset: Int): Int =
            ByteBuffer.wrap(this, offset, 4).order(ByteOrder.LITTLE_ENDIAN).int
    }
}

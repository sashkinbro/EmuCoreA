package com.sbro.emucorea.core

import android.content.Context
import android.graphics.BitmapFactory
import android.net.Uri
import java.io.Closeable
import java.io.File
import java.io.FileInputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.security.MessageDigest

data class PspGameMetadata(val title: String?, val serial: String?)

/**
 * Reads bounded PARAM.SFO and ICON0.PNG assets straight from the game image.
 *
 * The retired libretro frontend exposed PPSSPP's VFS asset reader; the native
 * core has no equivalent JNI, so plain ISO9660 images and PBP packages are
 * parsed here in Kotlin. Compressed containers (CSO/CHD/ZIP) fall back to
 * filename-derived metadata.
 */
object PspGameMetadataReader {

    private const val ASSET_PARAM_SFO = 0
    private const val ASSET_ICON0 = 1
    private const val ISO_SECTOR_BYTES = 2048
    private const val ISO_PRIMARY_VOLUME_DESCRIPTOR_SECTOR = 16
    private const val ISO_ROOT_RECORD_OFFSET = 156
    private const val ISO_DIRECTORY_ENTRY_BYTES = 34
    private const val MAX_ASSET_BYTES = 4 * 1024 * 1024
    private const val MAX_DIRECTORY_BYTES = 512 * 1024

    fun read(context: Context, path: String): PspGameMetadata? {
        val bytes = readAsset(context, path, ASSET_PARAM_SFO) ?: return null
        val fields = parseSfo(bytes)
        return PspGameMetadata(fields["TITLE"]?.takeIf(String::isNotBlank), fields["DISC_ID"]?.takeIf(String::isNotBlank))
    }

    fun extractIcon0(context: Context, path: String): String? = runCatching {
        val directory = File(context.cacheDir, "game-covers/embedded").apply { mkdirs() }
        val identity = if (path.startsWith("content://")) {
            val document = androidx.documentfile.provider.DocumentFile.fromSingleUri(context, Uri.parse(path))
            "$path:${document?.length()}:${document?.lastModified()}"
        } else {
            val file = File(path)
            "$path:${file.length()}:${file.lastModified()}"
        }
        val hash = MessageDigest.getInstance("SHA-256").digest(identity.toByteArray())
            .joinToString("") { "%02x".format(it) }
        val target = File(directory, "$hash.png")
        if (isImage(target)) return@runCatching target.absolutePath
        val bytes = readAsset(context, path, ASSET_ICON0) ?: return@runCatching null
        if (bytes.size < 8 || !bytes.copyOfRange(0, 8).contentEquals(byteArrayOf(0x89.toByte(), 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a))) {
            return@runCatching null
        }
        val temporary = File.createTempFile(hash, ".tmp", directory)
        try {
            temporary.writeBytes(bytes)
            if (!isImage(temporary)) return@runCatching null
            if (!temporary.renameTo(target)) temporary.copyTo(target, overwrite = true)
            target.takeIf(::isImage)?.absolutePath
        } finally {
            temporary.delete()
        }
    }.getOrNull()

    private fun readAsset(context: Context, path: String, asset: Int): ByteArray? =
        openSource(context, path)?.use { source ->
            runCatching {
                if (source.isPbp()) source.readPbpAsset(asset) else source.readIsoAsset(asset)
            }.getOrNull()
        }

    private fun openSource(context: Context, path: String): RandomAccessSource? = runCatching {
        if (path.startsWith("content://")) {
            val descriptor = context.contentResolver.openFileDescriptor(Uri.parse(path), "r")
                ?: return@runCatching null
            val stream = FileInputStream(descriptor.fileDescriptor)
            RandomAccessSource(stream.channel, descriptor.statSize.takeIf { it > 0 } ?: stream.channel.size(), descriptor)
        } else {
            val file = File(path)
            if (!file.isFile) return@runCatching null
            RandomAccessSource(RandomAccessFile(file, "r").channel, file.length())
        }
    }.getOrNull()

    private class RandomAccessSource(
        private val channel: FileChannel,
        val size: Long,
        private val owner: Closeable? = null
    ) : Closeable {

        fun read(offset: Long, length: Int): ByteArray? {
            if (offset < 0L || length <= 0 || offset + length > size) return null
            val buffer = ByteBuffer.allocate(length)
            var position = offset
            while (buffer.hasRemaining()) {
                val read = channel.read(buffer, position)
                if (read <= 0) return null
                position += read
            }
            return buffer.array()
        }

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
            val name = if (asset == ASSET_PARAM_SFO) "PARAM.SFO" else "ICON0.PNG"
            val descriptor = read(
                ISO_PRIMARY_VOLUME_DESCRIPTOR_SECTOR.toLong() * ISO_SECTOR_BYTES,
                ISO_SECTOR_BYTES
            ) ?: return null
            if (descriptor.size <= ISO_ROOT_RECORD_OFFSET || descriptor[0].toInt() != 1) return null
            val root = parseDirectoryRecord(descriptor, ISO_ROOT_RECORD_OFFSET) ?: return null
            val gameDirectory = findEntry(readDirectory(root), "PSP_GAME") ?: return null
            if (!gameDirectory.isDirectory) return null
            val entry = findEntry(readDirectory(gameDirectory), name) ?: return null
            if (entry.isDirectory || entry.length <= 0L || entry.length > MAX_ASSET_BYTES) return null
            return read(entry.extent * ISO_SECTOR_BYTES, entry.length.toInt())
        }

        private fun readDirectory(directory: IsoEntry): List<IsoEntry> {
            val length = directory.length.coerceAtMost(MAX_DIRECTORY_BYTES.toLong()).toInt()
            val data = read(directory.extent * ISO_SECTOR_BYTES, length) ?: return emptyList()
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

        override fun close() {
            runCatching { channel.close() }
            runCatching { owner?.close() }
        }
    }

    private data class IsoEntry(val name: String, val extent: Long, val length: Long, val isDirectory: Boolean)

    private fun findEntry(entries: List<IsoEntry>, name: String): IsoEntry? =
        entries.firstOrNull { it.name.equals(name, ignoreCase = true) }

    private fun parseSfo(bytes: ByteArray): Map<String, String> {
        if (bytes.size < 20 || !bytes.copyOfRange(0, 4).contentEquals(byteArrayOf(0, 0x50, 0x53, 0x46))) {
            return emptyMap()
        }
        val keyStart = littleInt(bytes, 8)
        val dataStart = littleInt(bytes, 12)
        val count = littleInt(bytes, 16)
        if (count !in 0..1024 || keyStart !in bytes.indices || dataStart !in bytes.indices) return emptyMap()
        val result = HashMap<String, String>()
        for (index in 0 until count) {
            val item = 20 + index * 16
            if (item + 16 > bytes.size) break
            val keyOffset = (bytes[item].toInt() and 0xff) or ((bytes[item + 1].toInt() and 0xff) shl 8)
            val valueLength = littleInt(bytes, item + 4)
            val valueOffset = dataStart.toLong() + (littleInt(bytes, item + 12).toLong() and 0xffffffffL)
            val keyAt = keyStart + keyOffset
            if (keyAt !in bytes.indices || valueLength !in 1..4096 || valueOffset < 0 || valueOffset + valueLength > bytes.size) continue
            val keyEnd = (keyAt until bytes.size).firstOrNull { bytes[it] == 0.toByte() } ?: continue
            val key = String(bytes, keyAt, keyEnd - keyAt, Charsets.UTF_8)
            if (key != "TITLE" && key != "DISC_ID") continue
            val value = String(bytes, valueOffset.toInt(), valueLength, Charsets.UTF_8).trimEnd('\u0000').trim()
            result[key] = value
        }
        return result
    }

    private fun littleInt(bytes: ByteArray, offset: Int): Int = ByteBuffer.wrap(bytes, offset, 4)
        .order(ByteOrder.LITTLE_ENDIAN).int

    private fun isImage(file: File): Boolean {
        if (!file.isFile || file.length() == 0L) return false
        val options = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(file.absolutePath, options)
        return options.outWidth > 0 && options.outHeight > 0
    }
}

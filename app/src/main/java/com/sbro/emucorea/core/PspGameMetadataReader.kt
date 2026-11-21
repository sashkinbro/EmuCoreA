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
import com.sbro.emucorea.xmb.XmbNative

data class PspGameMetadata(val title: String?, val serial: String?, val category: String?)

data class PspGameExtendedInfo(
    val title: String?,
    val serial: String?,
    val discVersion: String?,
    val systemVersion: String?,
    val parentalLevel: String?,
    val region: String?,
    val sizeBytes: Long,
    val lastModified: Long,
)

/**
 * Reads bounded PARAM.SFO and ICON0.PNG assets straight from the game image.
 *
 * The native core's loader stack is the primary reader: it opens every
 * container the emulator can boot (ISO, CSO, CHD and ZIP-wrapped images),
 * exactly like PPSSPP's own game list. When the library is absent (unit
 * tests), a Kotlin ISO9660/PBP/CSO parser takes over; CHD/ZIP fall back to
 * filename-derived metadata plus the cover index title match there.
 */
object PspGameMetadataReader {

    private val PNG_SIGNATURE = byteArrayOf(0x89.toByte(), 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)

    private fun resolveSizeAndModified(
        context: Context,
        path: String,
        knownSize: Long,
        knownModified: Long,
    ): Pair<Long, Long> {
        if (knownSize > 0L && knownModified > 0L) {
            return knownSize to knownModified
        }
        var sizeBytes = knownSize.coerceAtLeast(0L)
        var lastModified = knownModified.coerceAtLeast(0L)
        runCatching {
            if (path.startsWith("content://")) {
                val doc = androidx.documentfile.provider.DocumentFile.fromSingleUri(context, Uri.parse(path))
                if (sizeBytes <= 0L) sizeBytes = doc?.length() ?: 0L
                if (lastModified <= 0L) lastModified = doc?.lastModified() ?: 0L
            } else {
                val file = File(path)
                if (file.isFile) {
                    if (sizeBytes <= 0L) sizeBytes = file.length()
                    if (lastModified <= 0L) lastModified = file.lastModified()
                }
            }
        }
        return sizeBytes to lastModified
    }

    private fun assetHash(
        context: Context,
        path: String,
        knownSize: Long = -1L,
        knownModified: Long = -1L,
    ): String {
        val (sizeBytes, lastModified) = resolveSizeAndModified(context, path, knownSize, knownModified)
        val identity = "$path:$sizeBytes:$lastModified"
        return MessageDigest.getInstance("SHA-256").digest(identity.toByteArray())
            .joinToString("") { "%02x".format(it) }
    }

    fun read(context: Context, path: String): PspGameMetadata? {
        val bytes = readAsset(context, path, PSP_ASSET_PARAM_SFO) ?: return null
        val fields = parseSfo(bytes)
        return PspGameMetadata(
            title = fields["TITLE"]?.takeIf(String::isNotBlank),
            serial = fields["DISC_ID"]?.takeIf(String::isNotBlank),
            category = fields["CATEGORY"]?.takeIf(String::isNotBlank)
        )
    }

    fun readExtendedInfo(
        context: Context,
        path: String,
        knownSize: Long = -1L,
        knownModified: Long = -1L,
    ): PspGameExtendedInfo {
        val fields = runCatching {
            readAsset(context, path, PSP_ASSET_PARAM_SFO)?.let(::parseSfo)
        }.getOrNull().orEmpty()
        val (sizeBytes, lastModified) = resolveSizeAndModified(context, path, knownSize, knownModified)
        val rawSerial = fields["DISC_ID"]?.takeIf(String::isNotBlank)?.uppercase(java.util.Locale.ROOT)
        val formattedSerial = if (rawSerial != null && rawSerial.length == 9 && !rawSerial.contains('-')) {
            "${rawSerial.substring(0, 4)}-${rawSerial.substring(4)}"
        } else {
            rawSerial
        }
        val region = rawSerial?.let(::regionFromSerial)
        return PspGameExtendedInfo(
            title = fields["TITLE"]?.takeIf(String::isNotBlank),
            serial = formattedSerial,
            discVersion = fields["DISC_VERSION"]?.takeIf(String::isNotBlank),
            systemVersion = fields["PSP_SYSTEM_VER"]?.takeIf(String::isNotBlank),
            parentalLevel = fields["PARENTAL_LEVEL"]?.takeIf(String::isNotBlank),
            region = region,
            sizeBytes = sizeBytes,
            lastModified = lastModified,
        )
    }

    private fun regionFromSerial(serial: String): String? {
        if (serial.length < 4) return null
        return when (serial.substring(0, 4)) {
            "ULUS", "UCUS", "NPUG", "NPUH", "NPUZ", "NPUJ" -> "North America"
            "ULES", "UCES", "NPEG", "NPEH", "NPEZ" -> "Europe"
            "ULJM", "UCJM", "ULJS", "UCJS", "NPJG", "NPJH", "NPJJ" -> "Japan"
            "ULAS", "UCAS", "NPHG", "NPHH" -> "Asia"
            "ULKS", "UCKS", "NPKG", "NPKH" -> "Korea"
            else -> null
        }
    }

    private fun extractPngAsset(
        context: Context,
        path: String,
        assetId: Int,
        fileSuffix: String,
        noneSuffix: String,
        knownSize: Long,
        knownModified: Long,
    ): String? = runCatching {
        val directory = File(context.cacheDir, "game-covers/embedded").apply { mkdirs() }
        val hash = assetHash(context, path, knownSize, knownModified)
        val target = File(directory, "$hash$fileSuffix")
        if (target.isFile && target.length() > 24L) return@runCatching target.absolutePath
        val noneMarker = File(directory, "$hash$noneSuffix")
        if (noneMarker.isFile) return@runCatching null
        val bytes = readAsset(context, path, assetId)
        if (bytes == null) {
            runCatching { noneMarker.writeBytes(ByteArray(0)) }
            return@runCatching null
        }
        if (bytes.size < 8 || !bytes.copyOfRange(0, 8).contentEquals(PNG_SIGNATURE)) {
            runCatching { noneMarker.writeBytes(ByteArray(0)) }
            return@runCatching null
        }
        val temporary = File.createTempFile(hash, ".tmp", directory)
        try {
            temporary.writeBytes(bytes)
            if (!isImage(temporary)) {
                runCatching { noneMarker.writeBytes(ByteArray(0)) }
                return@runCatching null
            }
            if (!temporary.renameTo(target)) temporary.copyTo(target, overwrite = true)
            target.takeIf { it.isFile && it.length() > 24L }?.absolutePath
        } finally {
            temporary.delete()
        }
    }.getOrNull()

    fun extractIcon0(
        context: Context,
        path: String,
        knownSize: Long = -1L,
        knownModified: Long = -1L,
    ): String? = extractPngAsset(
        context = context,
        path = path,
        assetId = PSP_ASSET_ICON0,
        fileSuffix = ".png",
        noneSuffix = "_icon0.none",
        knownSize = knownSize,
        knownModified = knownModified,
    )

    fun extractPic1(
        context: Context,
        path: String,
        knownSize: Long = -1L,
        knownModified: Long = -1L,
    ): String? = extractPngAsset(
        context = context,
        path = path,
        assetId = PSP_ASSET_PIC1,
        fileSuffix = "_pic1.png",
        noneSuffix = "_pic1.none",
        knownSize = knownSize,
        knownModified = knownModified,
    )

    fun extractPic0(
        context: Context,
        path: String,
        knownSize: Long = -1L,
        knownModified: Long = -1L,
    ): String? = extractPngAsset(
        context = context,
        path = path,
        assetId = PSP_ASSET_PIC0,
        fileSuffix = "_pic0.png",
        noneSuffix = "_pic0.none",
        knownSize = knownSize,
        knownModified = knownModified,
    )

    /**
     * Checks whether SND0.AT3 has already been decoded or marked absent in the disk cache.
     * Returns:
     * - non-empty path if the decoded WAV file exists in cache
     * - `""` if a negative-cache `.none` marker exists (game has no SND0.AT3)
     * - `null` if SND0 has not been inspected/extracted yet
     */
    fun findCachedSnd0(
        context: Context,
        path: String,
        knownSize: Long = -1L,
        knownModified: Long = -1L,
    ): String? = runCatching {
        val directory = File(context.cacheDir, "game-covers/embedded")
        val hash = assetHash(context, path, knownSize, knownModified)
        val target = File(directory, "${hash}_snd0.wav")
        if (target.isFile && target.length() > 44L) return@runCatching target.absolutePath
        val noneMarker = File(directory, "${hash}_snd0.none")
        if (noneMarker.isFile) return@runCatching ""
        null
    }.getOrNull()

    fun extractSnd0(
        context: Context,
        path: String,
        knownSize: Long = -1L,
        knownModified: Long = -1L,
    ): String? = runCatching {
        val directory = File(context.cacheDir, "game-covers/embedded").apply { mkdirs() }
        val hash = assetHash(context, path, knownSize, knownModified)
        val target = File(directory, "${hash}_snd0.wav")
        if (target.isFile && target.length() > 44L) return@runCatching target.absolutePath
        val noneMarker = File(directory, "${hash}_snd0.none")
        if (noneMarker.isFile) return@runCatching null
        val bytes = readAsset(context, path, PSP_ASSET_SND0)
        if (bytes == null) {
            runCatching { noneMarker.writeBytes(ByteArray(0)) }
            return@runCatching null
        }
        if (bytes.size < 44 || !bytes.copyOfRange(0, 4).contentEquals("RIFF".toByteArray(Charsets.US_ASCII))) {
            runCatching { noneMarker.writeBytes(ByteArray(0)) }
            return@runCatching null
        }
        val temporary = File.createTempFile("${hash}_snd0", ".tmp", directory)
        try {
            val ok = XmbNative.nativeDecodeAt3Bytes(bytes, temporary.absolutePath)
            if (!ok || temporary.length() <= 44L) {
                runCatching { noneMarker.writeBytes(ByteArray(0)) }
                return@runCatching null
            }
            if (!temporary.renameTo(target)) temporary.copyTo(target, overwrite = true)
            target.takeIf { it.isFile && it.length() > 44L }?.absolutePath
        } finally {
            temporary.delete()
        }
    }.getOrNull()

    private fun readAsset(context: Context, path: String, asset: Int): ByteArray? =
        readAssetThroughCore(context, path, asset) ?: readAssetInProcess(context, path, asset)

    /**
     * Asks the native core to read the asset. The core's loader stack covers
     * every container it can boot, including CSO/ZSO/CHD images and ZIP files
     * wrapping an ISO, so those formats get real PARAM.SFO and ICON0.PNG data
     * without a Kotlin-side decoder. Returns null when the library is not
     * loaded (unit tests) or the core cannot open the container.
     */
    private fun readAssetThroughCore(context: Context, path: String, asset: Int): ByteArray? {
        if (!NativePpsspp.ensureLoaded()) return null
        return runCatching {
            if (path.startsWith("content://")) {
                context.contentResolver.openFileDescriptor(Uri.parse(path), "r")?.use { descriptor ->
                    NativePpsspp.nativeReadGameAsset(path, descriptor.fd, asset)
                }
            } else {
                NativePpsspp.nativeReadGameAsset(path, -1, asset)
            }
        }.getOrNull()
    }

    private fun readAssetInProcess(context: Context, path: String, asset: Int): ByteArray? =
        openSource(context, path)?.use { source ->
            runCatching {
                if (source.isPbp()) source.readPbpAsset(asset) else source.readIsoAsset(asset)
            }.getOrNull()
        }

    private fun openSource(context: Context, path: String): PspImageSource? = runCatching {
        if (path.startsWith("content://")) {
            val descriptor = context.contentResolver.openFileDescriptor(Uri.parse(path), "r")
                ?: return@runCatching null
            val stream = FileInputStream(descriptor.fileDescriptor)
            val source = RandomAccessSource(
                stream.channel,
                descriptor.statSize.takeIf { it > 0 } ?: stream.channel.size(),
                descriptor
            )
            CsoImageSource.open(source) ?: source
        } else {
            val file = File(path)
            if (!file.isFile) return@runCatching null
            val source = RandomAccessSource(RandomAccessFile(file, "r").channel, file.length())
            CsoImageSource.open(source) ?: source
        }
    }.getOrNull()

    private class RandomAccessSource(
        private val channel: FileChannel,
        override val size: Long,
        private val owner: Closeable? = null
    ) : PspImageSource {

        override fun read(offset: Long, length: Int): ByteArray? {
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

        override fun close() {
            runCatching { channel.close() }
            runCatching { owner?.close() }
        }
    }

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
            val dataFormat = (bytes[item + 2].toInt() and 0xff) or ((bytes[item + 3].toInt() and 0xff) shl 8)
            val valueLength = littleInt(bytes, item + 4)
            val valueOffset = dataStart.toLong() + (littleInt(bytes, item + 12).toLong() and 0xffffffffL)
            val keyAt = keyStart + keyOffset
            if (keyAt !in bytes.indices || valueLength !in 1..4096 || valueOffset < 0 || valueOffset + valueLength > bytes.size) continue
            val keyEnd = (keyAt until bytes.size).firstOrNull { bytes[it] == 0.toByte() } ?: continue
            val key = String(bytes, keyAt, keyEnd - keyAt, Charsets.UTF_8)
            if (key != "TITLE" && key != "DISC_ID" && key != "CATEGORY" &&
                key != "DISC_VERSION" && key != "PSP_SYSTEM_VER" && key != "PARENTAL_LEVEL") {
                continue
            }
            val value = if (dataFormat == 0x0404 && valueLength >= 4) {
                littleInt(bytes, valueOffset.toInt()).toString()
            } else {
                String(bytes, valueOffset.toInt(), valueLength, Charsets.UTF_8).trimEnd('\u0000').trim()
            }
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

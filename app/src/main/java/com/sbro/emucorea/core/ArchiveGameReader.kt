package com.sbro.emucorea.core

import android.content.Context
import android.os.ParcelFileDescriptor
import android.system.Os
import android.system.OsConstants
import android.util.Log
import androidx.core.net.toUri
import androidx.documentfile.provider.DocumentFile
import me.zhanghai.android.libarchive.Archive
import me.zhanghai.android.libarchive.ArchiveEntry
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.security.MessageDigest

/** Lists and materializes individual game files without loading an entire disc into RAM. */
object ArchiveGameReader {
    private const val TAG = "ArchiveGameReader"
    private const val MAX_ENTRIES = 20000
    private const val MAX_IMAGE_BYTES = 16L * 1024 * 1024 * 1024
    private val pinnedPaths = mutableSetOf<String>()
    data class Member(val name: String, val index: Int, val size: Long)

    fun list(context: Context, source: String, shouldAbort: () -> Boolean = { false }): List<Member> = try {
        withArchive(context, source, shouldAbort) { archive ->
            val members = mutableListOf<Member>()
            var index = 0
            while (index < MAX_ENTRIES && !shouldAbort()) {
                val entry = Archive.readNextHeader(archive)
                if (entry == 0L) break
                val name = entryName(entry)
                if (isRegular(entry) && !ArchiveEntry.isEncrypted(entry) && ArchiveGamePath.isPlayableMember(name)) {
                    members += Member(name, index, if (ArchiveEntry.sizeIsSet(entry)) ArchiveEntry.size(entry) else -1)
                }
                index++
            }
            members
        }
    } catch (e: Exception) {
        Log.w(TAG, "Cannot read archive: $source", e)
        emptyList()
    }

    @Synchronized
    fun prepare(context: Context, path: String, pinForLaunch: Boolean = false): String? {
        val member = ArchiveGamePath.parse(path) ?: return null
        return try {
            val dir = File(context.cacheDir, "archive-games").apply { mkdirs() }
            // prepare() is synchronized, so any leftover partial file belongs to an interrupted earlier run.
            dir.listFiles()?.filter { it.isFile && it.name.endsWith(".part") }?.forEach { it.delete() }
            val stamp = sourceStamp(context, member.source)
            // Providers without reliable timestamps cannot safely reuse (or replace) a running image.
            val version = stamp ?: java.util.UUID.randomUUID().toString()
            val key = MessageDigest.getInstance("SHA-256").digest("$path:$version".toByteArray()).joinToString("") { "%02x".format(it) }
            val extension = member.name.substringAfterLast('.', "bin").lowercase().filter { it.isLetterOrDigit() }.take(8).ifBlank { "bin" }
            val target = File(dir, "$key.$extension")
            if (stamp != null && target.isFile && target.length() > 0) {
                target.setLastModified(System.currentTimeMillis())
                if (pinForLaunch) pinnedPaths += target.absolutePath
                return target.absolutePath
            }
            trimCache(dir, target)
            val partial = File.createTempFile("extract-", ".part", dir)
            try {
                extractMember(context, member, partial)
                if (target.exists() && !target.delete()) throw IOException("Cannot replace cached image")
                if (!partial.renameTo(target)) throw IOException("Cannot publish cached image")
                if (pinForLaunch) pinnedPaths += target.absolutePath
                target.absolutePath
            } finally { partial.delete() }
        } catch (e: Exception) {
            Log.w(TAG, "Cannot prepare archived game: ${member.source}", e)
            null
        }
    }

    @Synchronized
    fun release(path: String?) {
        if (path != null) pinnedPaths.remove(path)
    }

    // Do not remove a path returned to a pending/running session. Keep the newest
    // unpinned image for quick restarts and reclaim older images on the next launch.
    private fun trimCache(dir: File, target: File) {
        if (EmulatorBridge.isVmActive()) return
        val candidates = dir.listFiles().orEmpty().filter {
            it.isFile && it != target && !it.name.endsWith(".part") && it.absolutePath !in pinnedPaths
        }.sortedByDescending { it.lastModified() }
        candidates.drop(1).forEach { it.delete() }
    }

    @Synchronized
    private fun reclaimSpace(context: Context, requiredBytes: Long) {
        if (EmulatorBridge.isVmActive()) return
        val dir = File(context.cacheDir, "archive-games")
        val candidates = dir.listFiles().orEmpty().filter {
            it.isFile && !it.name.endsWith(".part") && it.absolutePath !in pinnedPaths
        }.sortedBy { it.lastModified() }
        for (file in candidates) {
            if (dir.usableSpace >= requiredBytes) break
            file.delete()
        }
    }

    data class InspectedMetadata(val metadata: PspGameMetadata?, val iconPath: String?)

    fun inspect(context: Context, identity: String, shouldAbort: () -> Boolean): InspectedMetadata? {
        val member = ArchiveGamePath.parse(identity) ?: return null
        if (shouldAbort()) return null
        val extension = member.name.substringAfterLast('.', "bin").filter { it.isLetterOrDigit() }.take(8).ifBlank { "bin" }
        val file = File.createTempFile("archive-metadata-", ".$extension", context.cacheDir)
        return try {
            extractMember(context, member, file, shouldAbort)
            if (shouldAbort()) return null
            InspectedMetadata(PspGameMetadataReader.read(context, file.absolutePath),
                PspGameMetadataReader.extractIcon0(context, file.absolutePath))
        } catch (e: Exception) {
            Log.w(TAG, "Cannot inspect archived game: ${member.source}", e)
            null
        } finally { file.delete() }
    }

    private fun extractMember(context: Context, member: ArchiveGamePath.Entry, partial: File,
                              shouldAbort: () -> Boolean = { false }) {
        withArchive(context, member.source, shouldAbort) { archive ->
            var index = 0
            while (true) {
                val entry = Archive.readNextHeader(archive)
                if (entry == 0L || index >= MAX_ENTRIES) throw IOException("Archive member not found")
                if (index == member.index) {
                    if (entryName(entry) != member.name || !isRegular(entry) || ArchiveEntry.isEncrypted(entry)) {
                        throw IOException("Archive member changed or is encrypted")
                    }
                    val expected = if (ArchiveEntry.sizeIsSet(entry)) ArchiveEntry.size(entry) else -1L
                    if (expected > 0 && partial.parentFile!!.usableSpace < expected) reclaimSpace(context, expected)
                    if (expected == 0L || expected > MAX_IMAGE_BYTES ||
                        (expected > 0 && partial.parentFile!!.usableSpace < expected)) {
                        throw IOException("Not enough space or invalid image size")
                    }
                    FileOutputStream(partial).use { out ->
                        val buffer = ByteBuffer.allocate(128 * 1024)
                        var total = 0L
                        while (true) {
                            if (shouldAbort() || Thread.currentThread().isInterrupted) throw IOException("Extraction interrupted")
                            buffer.clear()
                            Archive.readData(archive, buffer)
                            val count = buffer.position()
                            if (count == 0) break
                            total += count
                            if (total > MAX_IMAGE_BYTES) throw IOException("Image exceeds size limit")
                            out.write(buffer.array(), 0, count)
                        }
                        if (total == 0L || (expected >= 0 && total != expected)) throw IOException("Incomplete image")
                    }
                    break
                }
                index++
            }
        }
    }

    private fun sourceStamp(context: Context, path: String): String? = if (path.startsWith("content://")) {
        val doc = DocumentFile.fromSingleUri(context, path.toUri())
        val size = doc?.length() ?: 0L
        val modified = doc?.lastModified() ?: 0L
        if (size > 0 && modified > 0) "$size:$modified" else null
    } else {
        val file = File(path)
        if (file.isFile) "${file.length()}:${file.lastModified()}" else null
    }

    private fun isRegular(entry: Long): Boolean = !ArchiveEntry.filetypeIsSet(entry) || ArchiveEntry.filetype(entry) == ArchiveEntry.AE_IFREG
    private fun entryName(entry: Long): String = ArchiveEntry.pathnameUtf8(entry) ?: ArchiveEntry.pathname(entry)?.toString(Charsets.UTF_8).orEmpty()

    private fun <T> withArchive(context: Context, source: String, shouldAbort: () -> Boolean = { false }, block: (Long) -> T): T {
        val accessibleSource = if (!source.startsWith("content://") && !File(source).canRead()) {
            DocumentPathResolver.findAccessibleTreeUriForRawPath(context, source)?.toString() ?: source
        } else source
        val descriptor = if (accessibleSource.startsWith("content://")) context.contentResolver.openFileDescriptor(accessibleSource.toUri(), "r")
            else ParcelFileDescriptor.open(File(accessibleSource), ParcelFileDescriptor.MODE_READ_ONLY)
        requireNotNull(descriptor) { "Cannot open archive" }
        descriptor.use { original ->
            val seekable = runCatching { Os.lseek(original.fileDescriptor, 0, OsConstants.SEEK_CUR) }.isSuccess
            if (seekable) return openArchive(original, block)
            val temp = File.createTempFile("archive-source-", ".part", context.cacheDir)
            try {
                ParcelFileDescriptor.AutoCloseInputStream(ParcelFileDescriptor.dup(original.fileDescriptor)).use { input ->
                    temp.outputStream().use { output ->
                        val buffer = ByteArray(128 * 1024)
                        var total = 0L
                        while (true) {
                            if (shouldAbort() || Thread.currentThread().isInterrupted) throw IOException("Archive read interrupted")
                            val count = input.read(buffer)
                            if (count < 0) break
                            total += count
                            if (total > MAX_IMAGE_BYTES) throw IOException("Archive exceeds size limit")
                            output.write(buffer, 0, count)
                        }
                    }
                }
                ParcelFileDescriptor.open(temp, ParcelFileDescriptor.MODE_READ_ONLY).use { return openArchive(it, block) }
            } finally { temp.delete() }
        }
    }

    private fun <T> openArchive(descriptor: ParcelFileDescriptor, block: (Long) -> T): T {
        val archive = Archive.readNew()
        try {
            Archive.readSupportFilterAll(archive)
            Archive.readSupportFormatZip(archive)
            Archive.readSupportFormat7zip(archive)
            Archive.readSupportFormatRar(archive)
            Archive.readSupportFormatRar5(archive)
            Archive.readOpenFd(archive, descriptor.fd, 65536)
            return block(archive)
        } finally { Archive.free(archive) }
    }
}

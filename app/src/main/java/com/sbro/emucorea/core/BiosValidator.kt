package com.sbro.emucorea.core

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import androidx.core.net.toUri
import androidx.documentfile.provider.DocumentFile
import java.io.File

object BiosValidator {

    // PPSSPP firmware: an EBOOT.PBP update or a dumped flash0 tree. There is no
    // fixed-size single-file PSP BIOS, so no size validation is applied.
    private val firmwareExtensions = setOf("pbp", "bin", "rom")
    private val firmwareDirectoryNames = setOf("flash0", "kd", "vsh")
    private val fileNameHints = listOf("bios", "firmware", "flash0", "psp update")
    private const val MAX_BIOS_PROBE_FILES = 24
    private const val MAX_BIOS_PROBE_DIRECTORIES = 96

    internal enum class DocumentEntryKind {
        DIRECTORY,
        BIOS_FILE,
        UNKNOWN,
        OTHER
    }

    fun hasUsableBiosFiles(context: Context, rawPath: String?): Boolean {
        if (rawPath.isNullOrBlank()) return false

        return if (rawPath.startsWith("content://")) {
            hasUsableContentBios(context, rawPath.toUri()) ||
                DocumentPathResolver.hasPreparedBiosForSource(context, rawPath)
        } else {
            val file = File(rawPath)
            when {
                file.isFile -> isValidLocalBios(file)
                file.isDirectory -> isFirmwareDirectory(file) ||
                    file.walkTopDown().maxDepth(2).any { it.isFile && isValidLocalBios(it) }
                else -> false
            }
        }
    }

    /** A folder that holds a PSP firmware dump (flash0 or its kd/vsh subfolders). */
    private fun isFirmwareDirectory(directory: File): Boolean {
        if (!directory.isDirectory) return false
        return firmwareDirectoryNames.any { name ->
            File(directory, name).isDirectory
        }
    }

    private fun hasUsableContentBios(context: Context, uri: Uri): Boolean = documentCheckOrFalse {
        if (DocumentsContract.isTreeUri(uri)) {
            val root = DocumentFile.fromTreeUri(context, uri) ?: return@documentCheckOrFalse false
            containsBiosFile(context, root, ProbeBudget())
        } else {
            val single = DocumentFile.fromSingleUri(context, uri) ?: return@documentCheckOrFalse false
            val displayName = documentDisplayName(context, single)
            isBiosCandidate(displayName) && isValidContentBios(context, single.uri, displayName)
        }
    }

    internal fun documentCheckOrFalse(check: () -> Boolean): Boolean {
        return try {
            check()
        } catch (_: RuntimeException) {
            false
        }
    }

    private fun containsBiosFile(context: Context, root: DocumentFile, budget: ProbeBudget): Boolean {
        if (!budget.tryEnterDirectory()) return false
        val children = runCatching { root.listFiles() }.getOrDefault(emptyArray())
        for (child in children) {
            val mimeType = runCatching { child.type }.getOrNull()
            val displayName = documentDisplayName(context, child)
            if (displayName.lowercase() in firmwareDirectoryNames) return true
            when (classifyDocumentEntry(mimeType, displayName)) {
                DocumentEntryKind.DIRECTORY,
                DocumentEntryKind.UNKNOWN -> {
                    // Some cloud and USB providers expose directories with a null MIME type.
                    // listFiles() is safe for an actual file and simply produces no children.
                    if (containsBiosFile(context, child, budget)) return true
                }
                DocumentEntryKind.BIOS_FILE -> {
                    if (!budget.tryCheckFile()) return false
                    if (isBiosCandidate(displayName) &&
                        isValidContentBios(context, child.uri, displayName)
                    ) {
                        return true
                    }
                }
                DocumentEntryKind.OTHER -> Unit
            }
        }
        return false
    }

    private fun documentDisplayName(context: Context, document: DocumentFile): String =
        runCatching { document.name }.getOrNull().orEmpty().ifBlank {
            runCatching { DocumentPathResolver.getDisplayName(context, document.uri.toString()) }
                .getOrDefault("")
        }

    private fun isReadable(context: Context, uri: Uri): Boolean = runCatching {
        context.contentResolver.openFileDescriptor(uri, "r")?.use { descriptor ->
            descriptor.statSize != 0L
        } ?: false
    }.getOrDefault(false)

    internal fun classifyDocumentEntry(mimeType: String?, displayName: String?): DocumentEntryKind {
        if (mimeType == DocumentsContract.Document.MIME_TYPE_DIR) return DocumentEntryKind.DIRECTORY
        if (isBiosCandidateName(displayName)) return DocumentEntryKind.BIOS_FILE
        return if (mimeType == null) DocumentEntryKind.UNKNOWN else DocumentEntryKind.OTHER
    }

    private fun isValidLocalBios(file: File): Boolean {
        if (!file.isFile || !isBiosCandidate(file.name)) return false
        val readable = file.canRead()
        if (!NativeApp.hasNativeCore) return readable
        return runCatching { NativeApp.isBiosPath(file.absolutePath) }.getOrDefault(false) || readable
    }

    private fun isValidContentBios(context: Context, uri: Uri, displayName: String): Boolean {
        if (!isBiosCandidate(displayName)) return false
        val readable = isReadable(context, uri)
        if (!NativeApp.hasNativeCore) return readable

        val descriptor = runCatching { context.contentResolver.openFileDescriptor(uri, "r") }.getOrNull()
            ?: return readable
        var detachedFd = -1
        return try {
            detachedFd = descriptor.detachFd()
            NativeApp.isBiosFd(detachedFd) || readable
        } catch (_: RuntimeException) {
            if (detachedFd >= 0) {
                runCatching { ParcelFileDescriptor.adoptFd(detachedFd).close() }
            }
            readable
        } finally {
            runCatching { descriptor.close() }
        }
    }

    private fun isBiosCandidate(name: String?): Boolean = isBiosCandidateName(name)

    private fun isBiosCandidateName(name: String?): Boolean {
        val extension = name.orEmpty().substringAfterLast('.', "").lowercase()
        return extension in firmwareExtensions
    }

    fun isLikelyBiosLibraryEntry(
        fileName: String,
        title: String?,
        serial: String?,
        fileSize: Long
    ): Boolean {
        val lowerFileName = fileName.lowercase()
        val lowerTitle = title.orEmpty().lowercase()
        val combined = "$lowerFileName $lowerTitle"
        return fileNameHints.any(combined::contains) ||
            combined.contains("playstation firmware") ||
            lowerFileName.startsWith("flash0") ||
            serial.orEmpty().lowercase().contains("firmware")
    }

    fun isLikelyBiosName(name: String?): Boolean {
        val fileName = name?.lowercase() ?: return false
        val ext = fileName.substringAfterLast('.', "")
        return ext in firmwareExtensions && fileNameHints.any(fileName::contains)
    }

    internal fun isUsableMainBiosImage(name: String?, fileSize: Long): Boolean =
        isBiosCandidateName(name)

    private class ProbeBudget {
        private var checkedFiles = 0
        private var checkedDirectories = 0

        fun tryCheckFile(): Boolean {
            if (checkedFiles >= MAX_BIOS_PROBE_FILES) return false
            checkedFiles++
            return true
        }

        fun tryEnterDirectory(): Boolean {
            if (checkedDirectories >= MAX_BIOS_PROBE_DIRECTORIES) return false
            checkedDirectories++
            return true
        }
    }
}

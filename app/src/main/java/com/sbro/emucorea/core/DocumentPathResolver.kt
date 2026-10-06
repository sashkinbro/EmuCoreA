package com.sbro.emucorea.core

import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.OpenableColumns
import android.provider.DocumentsContract
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import java.io.File
import androidx.core.net.toUri

object DocumentPathResolver {
    private const val TAG = "DocumentPathResolver"

    fun resolveFilePath(context: Context, rawPath: String): String? {
        if (!rawPath.startsWith("content://")) return rawPath

        val uri = rawPath.toUri()
        val directPath = resolveExternalStoragePath(uri)
        if (directPath != null) return directPath

        val fileName = DocumentFile.fromSingleUri(context, uri)?.name ?: return null
        return findFileInPersistedTree(context, uri, fileName)
    }

    fun resolveDirectoryPath(rawPath: String): String? {
        if (!rawPath.startsWith("content://")) return rawPath
        return resolveExternalStoragePath(rawPath.toUri())
    }

    fun findAccessibleTreeUriForRawPath(context: Context, rawPath: String): Uri? {
        if (rawPath.startsWith("content://")) return rawPath.toUri()

        val normalizedRawPath = File(rawPath).absolutePath.removeSuffix("/")
        val persistedTrees = context.contentResolver.persistedUriPermissions
            .mapNotNull { permission ->
                val treeUri = permission.uri
                val treePath = resolveExternalStoragePath(treeUri)?.removeSuffix("/") ?: return@mapNotNull null
                treeUri to treePath
            }
            .sortedByDescending { (_, treePath) -> treePath.length }

        for ((treeUri, treePath) in persistedTrees) {
            if (normalizedRawPath != treePath && !normalizedRawPath.startsWith("$treePath/")) {
                continue
            }

            if (normalizedRawPath == treePath) {
                return treeUri
            }

            val root = DocumentFile.fromTreeUri(context, treeUri) ?: continue
            val relativeSegments = normalizedRawPath
                .removePrefix(treePath)
                .trim('/')
                .split('/')
                .filter { it.isNotBlank() }

            var current = root
            var failed = false
            for (segment in relativeSegments) {
                current = current.findFile(segment) ?: run {
                    failed = true
                    break
                }
            }
            if (!failed) {
                return current.uri
            }
        }

        return null
    }

    fun isScopedStorageExternalPath(rawPath: String): Boolean {
        if (rawPath.startsWith("content://")) return false
        val normalized = File(rawPath).absolutePath
        val primaryExternal = Environment.getExternalStorageDirectory().absolutePath
        return normalized.startsWith(primaryExternal)
    }

    fun getDisplayName(context: Context, rawPath: String): String {
        if (!rawPath.startsWith("content://")) return normalizeDisplayName(rawPath)

        val uri = rawPath.toUri()
        val fromResolver = runCatching {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { cursor ->
                    if (cursor.moveToFirst()) {
                        cursor.getString(0)
                    } else {
                        null
                    }
                }
        }.getOrNull()
        if (!fromResolver.isNullOrBlank()) return normalizeDisplayName(fromResolver, uri)

        val fromSingle = runCatching { DocumentFile.fromSingleUri(context, uri)?.name }.getOrNull()
        if (!fromSingle.isNullOrBlank()) return normalizeDisplayName(fromSingle, uri)

        val fromTree = runCatching {
            if (DocumentsContract.isTreeUri(uri)) DocumentFile.fromTreeUri(context, uri)?.name else null
        }.getOrNull()
        if (!fromTree.isNullOrBlank()) return normalizeDisplayName(fromTree, uri)

        return normalizeDisplayName(rawPath, uri)
    }

    /**
     * Produces a useful label from a persisted path without querying a DocumentsProvider.
     * Safe for Compose and other main-thread UI code where a provider query can block Binder.
     */
    fun getFallbackDisplayName(rawPath: String): String {
        if (!rawPath.startsWith("content://")) return normalizeDisplayName(rawPath)
        return normalizeDisplayName(rawPath, rawPath.toUri())
    }

    fun prepareElfLaunchPath(context: Context, rawPath: String): String? {
        if (rawPath.isBlank()) return null
        if (!rawPath.startsWith("content://")) return File(rawPath).takeIf { it.isFile && it.canRead() }?.absolutePath ?: rawPath

        val uri = rawPath.toUri()
        val single = DocumentFile.fromSingleUri(context, uri)
        val displayName = single?.name ?: getDisplayName(context, rawPath)
        if (!displayName.substringAfterLast('.', "").equals("elf", ignoreCase = true)) {
            return rawPath
        }

        val directPath = resolveFilePath(context, rawPath)
            ?.let(::File)
            ?.takeIf { it.isFile && it.canRead() }
            ?.absolutePath
        if (!directPath.isNullOrBlank()) return directPath

        return uri.toString()
    }

    fun prepareGameLaunchPath(context: Context, rawPath: String): String? {
        if (rawPath.isBlank()) return null
        if (!rawPath.startsWith("content://")) {
            val direct = File(rawPath)
            if (direct.isFile && direct.canRead()) return direct.absolutePath

            val uri = findAccessibleTreeUriForRawPath(context, rawPath)
                ?: return if (isScopedStorageExternalPath(rawPath)) null else rawPath
            return prepareUriGameLaunchPath(context, uri)
        }

        val uri = rawPath.toUri()
        val directPath = resolveFilePath(context, rawPath)
            ?.let(::File)
            ?.takeIf { it.isFile && it.canRead() }
            ?.absolutePath
        if (!directPath.isNullOrBlank()) return directPath

        return prepareUriGameLaunchPath(context, uri)
    }

    private fun prepareUriGameLaunchPath(context: Context, uri: Uri): String? {
        val resolvedDirect = resolveFilePath(context, uri.toString())
            ?.let(::File)
            ?.takeIf { it.isFile && it.canRead() }
            ?.absolutePath
        if (!resolvedDirect.isNullOrBlank()) {
            Log.i(TAG, "Resolved content URI to direct filesystem game path: $resolvedDirect")
            return resolvedDirect
        }

        return uri.toString()
    }

    private fun resolveExternalStoragePath(uri: Uri): String? {
        val documentId = runCatching { DocumentsContract.getDocumentId(uri) }.getOrNull()
            ?: runCatching { DocumentsContract.getTreeDocumentId(uri) }.getOrNull()
            ?: return null

        val parts = Uri.decode(documentId).split(':', limit = 2)
        if (parts.isEmpty()) return null

        val volume = parts[0]
        val relativePath = parts.getOrNull(1).orEmpty()

        return when {
            volume.equals("primary", ignoreCase = true) -> {
                val base = Environment.getExternalStorageDirectory()
                if (relativePath.isBlank()) base.absolutePath
                else File(base, relativePath).absolutePath
            }
            volume.equals("home", ignoreCase = true) -> {
                val base = File(Environment.getExternalStorageDirectory(), "Documents")
                if (relativePath.isBlank()) base.absolutePath
                else File(base, relativePath).absolutePath
            }
            volume.equals("raw", ignoreCase = true) && relativePath.startsWith("/") -> relativePath
            volume.startsWith("/") -> volume
            else -> {
                val base = File("/storage", volume)
                if (relativePath.isBlank()) base.absolutePath
                else File(base, relativePath).absolutePath
            }
        }
    }

    private fun findFileInPersistedTree(context: Context, targetUri: Uri, fileName: String): String? {
        val persistedTrees = context.contentResolver.persistedUriPermissions
            .mapNotNull { permission -> DocumentFile.fromTreeUri(context, permission.uri) }

        for (tree in persistedTrees) {
            val resolved = findFileRecursive(tree, targetUri, fileName)
            if (resolved != null) return resolved
        }

        return null
    }

    private fun findFileRecursive(root: DocumentFile, targetUri: Uri, fileName: String): String? {
        for (child in root.listFiles()) {
            if (child.uri == targetUri) {
                return resolveExternalStoragePath(child.uri)
            }

            if (child.isDirectory) {
                val nested = findFileRecursive(child, targetUri, fileName)
                if (nested != null) return nested
            } else if (child.name == fileName) {
                val direct = resolveExternalStoragePath(child.uri)
                if (direct != null) return direct
            }
        }

        return null
    }

    fun normalizeDisplayName(rawName: String): String {
        return normalizeDisplayName(rawName, null)
    }

    private fun normalizeDisplayName(rawName: String, uri: Uri?): String {
        val candidates = buildList {
            add(rawName)
            if (uri != null) {
                runCatching { DocumentsContract.getDocumentId(uri) }.getOrNull()?.let(::add)
                runCatching { DocumentsContract.getTreeDocumentId(uri) }.getOrNull()?.let(::add)
                uri.lastPathSegment?.let(::add)
            }
        }

        for (candidate in candidates) {
            val name = extractDisplayLeaf(candidate)
            if (name.isNotBlank()) return name
        }

        return rawName
    }

    private fun extractDisplayLeaf(candidate: String): String {
        val decoded = Uri.decode(candidate).orEmpty().trim()
        if (decoded.isBlank()) return ""

        val withoutRawPrefix = decoded.removePrefix("raw:")
        val documentPart = withoutRawPrefix.substringAfter("/document/", withoutRawPrefix)
        val treePart = documentPart.substringAfter("/tree/", documentPart)
        val storagePart = when {
            treePart.startsWith("primary:", ignoreCase = true) -> treePart.substringAfter(':')
            treePart.startsWith("home:", ignoreCase = true) -> treePart.substringAfter(':')
            treePart.indexOf(':') > 0 && treePart.substringAfter(':').contains('/') -> treePart.substringAfter(':')
            else -> treePart
        }
        return storagePart.substringAfterLast('/').trim()
    }
}

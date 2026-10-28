package com.sbro.emucorea.data

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import androidx.core.net.toUri
import androidx.documentfile.provider.DocumentFile
import com.sbro.emucorea.core.ArchiveGamePath
import com.sbro.emucorea.core.ArchiveGameReader
import com.sbro.emucorea.core.BiosValidator
import com.sbro.emucorea.core.DocumentPathResolver
import com.sbro.emucorea.core.EmulatorBridge
import com.sbro.emucorea.core.SetupValidator
import com.sbro.emucorea.core.PspGameMetadataReader
import com.sbro.emucorea.data.psp.PspTitleIndexRepository
import java.io.File

data class GameItem(
    val title: String,
    val path: String,
    val fileName: String,
    val fileSize: Long,
    val lastModified: Long,
    val coverArtPath: String? = null,
    val serial: String? = null,
    val isDlc: Boolean = false,
    val archiveMetadataRead: Boolean = false
)

class GameRepository {

    companion object {
        private const val TAG = "GameRepository"
        // Keep the library honest with what the bundled PPSSPP core mounts.
        private val COVER_EXTENSIONS = setOf("jpg", "jpeg", "png", "webp")
        private val COVER_DIRECTORY_NAMES = setOf("covers", "cover", "art", "artwork", "boxart", "box art")
        private const val MAX_DOCUMENT_SCAN_DEPTH = 32
        private const val MAX_DOCUMENT_SCAN_ENTRIES = 20_000
        internal fun libraryIdentity(path: String): String {
            ArchiveGamePath.parse(path)?.let { return ArchiveGamePath.create(libraryIdentity(it.source), it.name, it.index) }
            if (!path.startsWith("content://")) {
                return runCatching { File(path).canonicalPath }
                    .getOrDefault(File(path).absolutePath)
            }
            return runCatching {
                val uri = path.toUri()
                val documentId = DocumentsContract.getDocumentId(uri)
                "${uri.authority}:$documentId"
            }.getOrDefault(path)
        }

        internal fun parentDocumentId(documentId: String): String? {
            val separatorIndex = documentId.lastIndexOf('/')
            if (separatorIndex >= 0) return documentId.substring(0, separatorIndex)

            val volumeSeparatorIndex = documentId.indexOf(':')
            return if (volumeSeparatorIndex >= 0 && volumeSeparatorIndex < documentId.lastIndex) {
                documentId.substring(0, volumeSeparatorIndex + 1)
            } else {
                null
            }
        }

        internal fun relativeDocumentSegments(rootDocumentId: String, targetDocumentId: String): List<String>? {
            if (targetDocumentId == rootDocumentId) return emptyList()
            val prefix = if (rootDocumentId.endsWith(':')) rootDocumentId else "$rootDocumentId/"
            if (!targetDocumentId.startsWith(prefix)) return null
            return targetDocumentId.removePrefix(prefix).split('/').filter(String::isNotBlank)
        }

        /**
         * PSP DLC packages carry CATEGORY "AC" (additional content) in PARAM.SFO.
         * Some packages keep the game category, so fall back to the generic
         * localized "downloadable content" titles, the ubiquitous standalone
         * "DLC" marker and add-on/update packaging titles those files ship with.
         */
        private val DLC_TOKEN = Regex("""(^|[^a-z])dlc([^a-z]|$)""")
        private val ADDON_TOKEN = Regex("""(^|[^a-z])add[ -]?ons?([^a-z]|$)""")

        internal fun isDlcMetadata(category: String?, title: String?, fileName: String? = null): Boolean {
            if (category?.trim().equals("AC", ignoreCase = true)) return true
            val candidates = sequenceOf(title, fileName)
                .filterNotNull()
                .map(String::trim)
                .filter(String::isNotEmpty)
            return candidates.any { value ->
                val normalized = value.lowercase()
                normalized.contains("downloadable content") ||
                    normalized.contains("загружаемый контент") ||
                    normalized.contains("追加コンテンツ") ||
                    normalized.contains("追加データ") ||
                    normalized.contains("アップデート") ||
                    normalized.contains("다운로드 콘텐츠") ||
                    normalized.contains("추가 콘텐츠") ||
                    normalized.contains("update ver") ||
                    DLC_TOKEN.containsMatchIn(normalized) ||
                    ADDON_TOKEN.containsMatchIn(normalized)
            }
        }
    }

    fun scanDirectory(path: String, context: Context): List<GameItem> {
        return scanDirectory(path, context, emptyMap())
    }

    fun scanDirectory(
        path: String,
        context: Context,
        cachedGamesByPath: Map<String, GameItem>,
        shouldAbort: () -> Boolean = { false },
        preferEnglishTitles: Boolean = false
    ): List<GameItem> {
        val dir = File(path)
        if (!dir.exists() || !dir.isDirectory) return emptyList()
        return scanLocalDirectory(
            dir,
            context,
            cachedGamesByPath,
            shouldAbort,
            preferEnglishTitles
        ).sortedBy { it.title.lowercase() }
    }

    fun scanDirectoryFromUri(uri: Uri, context: Context): List<GameItem> {
        return scanDirectoryFromUri(uri, context, emptyMap())
    }

    fun scanDirectoryFromUri(
        uri: Uri,
        context: Context,
        cachedGamesByPath: Map<String, GameItem>,
        shouldAbort: () -> Boolean = { false },
        preferEnglishTitles: Boolean = false
    ): List<GameItem> {
        if (!DocumentsContract.isTreeUri(uri)) {
            return emptyList()
        }
        return runCatching {
            val docFile = DocumentFile.fromTreeUri(context, uri) ?: return@runCatching emptyList()
            scanDocumentFile(
                docFile,
                context,
                cachedGamesByPath,
                shouldAbort,
                preferEnglishTitles = preferEnglishTitles,
                depth = 0,
                budget = DocumentScanBudget()
            )
                .sortedBy { it.title.lowercase() }
        }.onFailure { error ->
            Log.w(TAG, "Unable to scan document tree: $uri", error)
        }.getOrDefault(emptyList())
    }

    fun findCoverForGame(
        path: String,
        context: Context,
        serial: String? = null,
        title: String? = null
    ): String? {
        val archive = ArchiveGamePath.parse(path)
        if (archive != null || ArchiveGamePath.isArchiveName(DocumentPathResolver.getDisplayName(context, path))) {
            CustomGameCoverRepository(context).findCustomCoverPath(path)?.let { return it }
            val source = archive?.source ?: path
            val memberName = archive?.name?.replace('\\', '/')?.substringAfterLast('/')
                ?: DocumentPathResolver.getDisplayName(context, source)
            val candidate = if (source.startsWith("content://")) {
                runCatching {
                    val parent = findDocumentParent(context, source.toUri()) ?: return@runCatching null
                    val covers = buildDocumentCoverCandidates(parent.listFiles())
                    covers[normalizeBaseName(memberName.substringBeforeLast('.'))]?.uri?.toString()
                        ?: covers[normalizeBaseName(cleanGameName(title.orEmpty()))]?.uri?.toString()
                }.getOrNull()
            } else {
                val parent = File(source).parentFile
                val covers = buildLocalCoverCandidates(parent?.listFiles().orEmpty())
                covers[normalizeBaseName(memberName.substringBeforeLast('.'))]?.absolutePath
                    ?: covers[normalizeBaseName(cleanGameName(title.orEmpty()))]?.absolutePath
            }
            return CoverArtRepository(context).displayCoverPath(serial, title, candidate)
        }
        val candidate = if (path.startsWith("content://")) {
            runCatching { findDocumentCover(path, context, serial, title) }
                .onFailure { error -> Log.w(TAG, "Unable to find cover for document URI: $path", error) }
                .getOrNull()
        } else {
            findLocalCover(path, context, serial, title)
        }
        return CoverArtRepository(context).displayCoverPath(serial, title, candidate)
    }

    fun downloadCoverForGame(game: GameItem, context: Context): String? {
        CustomGameCoverRepository(context).findCustomCoverPath(game.path)?.let { return it }
        // Retail identity may come from the disc serial or, for compressed images whose
        // PARAM.SFO is unreadable, from the bundled title index; a loose homebrew ELF matches neither.
        val covers = CoverArtRepository(context)
        val downloaded = covers.downloadCover(game.serial, title = game.title)
        if (covers.hasCoverSource(game.serial, game.title)) return downloaded
        return downloaded
            ?: game.coverArtPath?.takeIf { File(it).isFile }
            ?: game.path.takeUnless { ArchiveGamePath.parse(it) != null || ArchiveGamePath.isArchiveName(DocumentPathResolver.getDisplayName(context, it)) }?.let { PspGameMetadataReader.extractIcon0(context, it) }
    }

    private fun scanLocalDirectory(
        dir: File,
        context: Context,
        cachedGamesByPath: Map<String, GameItem>,
        shouldAbort: () -> Boolean,
        preferEnglishTitles: Boolean = false
    ): List<GameItem> {
        val items = mutableListOf<GameItem>()
        val children = dir.listFiles().orEmpty()
        val coverCandidates = buildLocalCoverCandidates(children)
        val coverRepository = CoverArtRepository(context)
        val customCoverRepository = CustomGameCoverRepository(context)
        val titleIndex = PspTitleIndexRepository(context)

        children.forEach { file ->
            if (shouldAbort()) return items
            when {
                file.isDirectory && normalizeBaseName(file.name) !in COVER_DIRECTORY_NAMES -> {
                    items.addAll(
                        scanLocalDirectory(
                            file,
                            context,
                            cachedGamesByPath,
                            shouldAbort,
                            preferEnglishTitles
                        )
                    )
                }

                file.isFile && com.sbro.emucorea.core.PspGameFormats.isSupportedName(file.name) -> {
                    if (ArchiveGamePath.isArchiveName(file.name)) {
                        items += scanArchive(context, file.absolutePath, file.length(), file.lastModified(), cachedGamesByPath, shouldAbort, file.extension.equals("zip", true), preferEnglishTitles) { name, title ->
                            coverCandidates[normalizeBaseName(name.substringBeforeLast('.'))]?.absolutePath
                                ?: coverCandidates[normalizeBaseName(cleanGameName(title))]?.absolutePath
                        }
                        return@forEach
                    }
                    val entrySize = file.length()
                    val cachedGame = cachedGamesByPath[file.absolutePath]
                    val canReuseCachedMetadata = cachedGame != null &&
                        cachedGame.fileSize == entrySize &&
                        cachedGame.lastModified == file.lastModified() &&
                        cachedGame.fileName == file.name &&
                        !cachedGame.serial.isNullOrBlank()
                    val metadata: com.sbro.emucorea.core.GameMetadata
                    val isDlc: Boolean
                    if (canReuseCachedMetadata) {
                        metadata = com.sbro.emucorea.core.GameMetadata(cachedGame.title, cachedGame.serial)
                        isDlc = cachedGame.isDlc
                    } else {
                        val sourcePath = file.absolutePath
                        val sourceMetadata = EmulatorBridge.getGameMetadata(
                            sourcePath,
                            readDiscMetadata = false
                        )
                        val pspMetadata = PspGameMetadataReader.read(context, sourcePath)
                        metadata = sourceMetadata.copy(
                            title = pspMetadata?.title ?: sourceMetadata.title,
                            serial = pspMetadata?.serial ?: sourceMetadata.serial
                        )
                        isDlc = isDlcMetadata(pspMetadata?.category, metadata.title, file.name)
                    }
                    if (BiosValidator.isLikelyBiosLibraryEntry(file.name, metadata.title, metadata.serial)) {
                        return@forEach
                    }

                    val cleanTitle = EmulatorBridge.cleanGameDisplayTitle(metadata.title, file.name)
                    val serial = metadata.serial
                    val title = if (preferEnglishTitles) {
                        titleIndex.titleForSerial(serial) ?: cleanTitle
                    } else {
                        cleanTitle
                    }
                    items += GameItem(
                        title = title,
                        path = file.absolutePath,
                        fileName = file.name,
                        fileSize = entrySize,
                        lastModified = file.lastModified(),
                        coverArtPath = customCoverRepository.findCustomCoverPath(file.absolutePath)
                            ?: coverRepository.findCachedCoverPath(serial, title = title)
                            ?: PspGameMetadataReader.extractIcon0(context, file.absolutePath)
                            ?: cachedGame?.coverArtPath?.takeIf { File(it).exists() }
                            ?: coverCandidates[normalizeBaseName(file.nameWithoutExtension)]?.absolutePath
                            ?: coverCandidates[normalizeBaseName(cleanGameName(title))]?.absolutePath,
                        serial = serial,
                        isDlc = isDlc
                    )
                }
            }
        }

        return items
    }

    private fun scanDocumentFile(
        docFile: DocumentFile,
        context: Context,
        cachedGamesByPath: Map<String, GameItem>,
        shouldAbort: () -> Boolean,
        preferEnglishTitles: Boolean = false,
        depth: Int,
        budget: DocumentScanBudget
    ): List<GameItem> {
        if (depth > MAX_DOCUMENT_SCAN_DEPTH || !budget.tryEnterDirectory(docFile.uri.toString())) {
            return emptyList()
        }
        val items = mutableListOf<GameItem>()
        val children = runCatching { docFile.listFiles() }.getOrDefault(emptyArray())
        val coverCandidates = buildDocumentCoverCandidates(children)
        val coverRepository = CoverArtRepository(context)
        val customCoverRepository = CustomGameCoverRepository(context)
        val titleIndex = PspTitleIndexRepository(context)

        for (file in children) {
            if (shouldAbort() || !budget.tryVisitEntry()) return items
            val mimeType = runCatching { file.type }.getOrNull()
            val name = documentDisplayName(context, file)
            when (SetupValidator.classifyDocumentEntry(mimeType, name)) {
                SetupValidator.DocumentEntryKind.DIRECTORY,
                SetupValidator.DocumentEntryKind.UNKNOWN -> {
                    if (normalizeBaseName(name) !in COVER_DIRECTORY_NAMES) {
                        items.addAll(
                            scanDocumentFile(
                                file,
                                context,
                                cachedGamesByPath,
                                shouldAbort,
                                preferEnglishTitles,
                                depth + 1,
                                budget
                            )
                        )
                    }
                }
                SetupValidator.DocumentEntryKind.GAME_FILE -> {
                    val uriPath = file.uri.toString()
                    val fileSize = runCatching { file.length() }.getOrDefault(0L)
                    if (ArchiveGamePath.isArchiveName(name)) {
                        val modified = runCatching { file.lastModified() }.getOrDefault(0L)
                        items += scanArchive(context, uriPath, fileSize, modified, cachedGamesByPath, shouldAbort, name.endsWith(".zip", true), preferEnglishTitles) { memberName, title ->
                            coverCandidates[normalizeBaseName(memberName.substringBeforeLast('.'))]?.uri?.toString()
                                ?: coverCandidates[normalizeBaseName(cleanGameName(title))]?.uri?.toString()
                        }
                        continue
                    }
                    val entrySize = fileSize
                    val lastModified = runCatching { file.lastModified() }.getOrDefault(0L)

                    val cachedGame = cachedGamesByPath[uriPath]
                    val canReuseCachedMetadata = cachedGame != null &&
                        cachedGame.fileSize == entrySize &&
                        cachedGame.lastModified == lastModified &&
                        cachedGame.fileName == name &&
                        !cachedGame.serial.isNullOrBlank()
                    val metadata: com.sbro.emucorea.core.GameMetadata
                    val isDlc: Boolean
                    if (canReuseCachedMetadata) {
                        metadata = com.sbro.emucorea.core.GameMetadata(cachedGame.title, cachedGame.serial)
                        isDlc = cachedGame.isDlc
                    } else {
                        val sourcePath = uriPath
                        val sourceMetadata = EmulatorBridge.getGameMetadata(
                            sourcePath,
                            readDiscMetadata = false
                        )
                        val pspMetadata = PspGameMetadataReader.read(context, sourcePath)
                        metadata = sourceMetadata.copy(
                            title = pspMetadata?.title ?: sourceMetadata.title,
                            serial = pspMetadata?.serial ?: sourceMetadata.serial
                        )
                        isDlc = isDlcMetadata(pspMetadata?.category, metadata.title, name)
                    }

                    if (BiosValidator.isLikelyBiosLibraryEntry(name, metadata.title, metadata.serial)) {
                        continue
                    }

                    val cleanTitle = cleanScannedTitle(metadata.title, name)
                    val serial = metadata.serial
                    val title = if (preferEnglishTitles) {
                        titleIndex.titleForSerial(serial) ?: cleanTitle
                    } else {
                        cleanTitle
                    }
                    items += GameItem(
                        title = title,
                        path = uriPath,
                        fileName = name,
                        fileSize = entrySize,
                        lastModified = lastModified,
                        coverArtPath = customCoverRepository.findCustomCoverPath(uriPath)
                            ?: coverRepository.findCachedCoverUri(serial, title = title)
                            ?: PspGameMetadataReader.extractIcon0(context, uriPath)
                            ?: cachedGame?.coverArtPath
                            ?: coverCandidates[normalizeBaseName(name.substringBeforeLast('.'))]?.uri?.toString()
                            ?: coverCandidates[normalizeBaseName(cleanGameName(title))]?.uri?.toString(),
                        serial = serial,
                        isDlc = isDlc
                    )
                }
                SetupValidator.DocumentEntryKind.OTHER -> Unit
            }
        }

        return items
    }

    private fun scanArchive(
        context: Context,
        source: String,
        size: Long,
        modified: Long,
        cachedGamesByPath: Map<String, GameItem>,
        shouldAbort: () -> Boolean,
        legacyZip: Boolean,
        preferEnglishTitles: Boolean,
        siblingCover: (String, String) -> String?
    ): List<GameItem> {
        val covers = CoverArtRepository(context)
        val customCovers = CustomGameCoverRepository(context)
        val titleIndex = PspTitleIndexRepository(context)
        val members = ArchiveGameReader.list(context, source, shouldAbort)
        val legacyIndex = if (legacyZip) ArchiveGamePath.legacyZipMemberIndex(members.map { it.name }) else -1
        return members.mapIndexedNotNull { position, member ->
            val path = if (position == legacyIndex) source else ArchiveGamePath.create(source, member.name, member.index)
            val name = member.name.replace('\\', '/').substringAfterLast('/')
            val cached = cachedGamesByPath[path]?.takeIf { it.fileSize == size && it.lastModified == modified }
            val inferred = EmulatorBridge.getGameMetadata(name, readDiscMetadata = false)
            val identity = ArchiveGamePath.create(source, member.name, member.index)
            val inspected = if (cached?.archiveMetadataRead != true && !shouldAbort()) ArchiveGameReader.inspect(context, identity, shouldAbort) else null
            val serial = inspected?.metadata?.serial ?: cached?.serial ?: inferred.serial
            val cleanTitle = EmulatorBridge.cleanGameDisplayTitle(inspected?.metadata?.title ?: cached?.title ?: inferred.title, name)
            val title = if (preferEnglishTitles) titleIndex.titleForSerial(serial) ?: cleanTitle else cleanTitle
            if (BiosValidator.isLikelyBiosLibraryEntry(name, title, serial)) return@mapIndexedNotNull null
            GameItem(
                title = title, path = path, fileName = name, fileSize = size, lastModified = modified,
                coverArtPath = customCovers.findCustomCoverPath(path)
                    ?: covers.findCachedCoverPath(serial, title = title)
                    ?: inspected?.iconPath
                    ?: cached?.coverArtPath
                    ?: siblingCover(name, title),
                serial = serial, isDlc = if (inspected != null) isDlcMetadata(inspected.metadata?.category, title, name) else cached?.isDlc ?: isDlcMetadata(null, title, name),
                archiveMetadataRead = inspected != null || cached?.archiveMetadataRead == true
            )
        }
    }

    private fun findLocalCover(path: String, context: Context, serial: String?, title: String?): String? {
        val file = File(path)
        val parent = file.parentFile ?: return null
        val baseName = normalizeBaseName(file.nameWithoutExtension)
        val titleKey = normalizeBaseName(cleanGameName(title ?: EmulatorBridge.getGameTitle(path)))
        val coverCandidates = buildLocalCoverCandidates(parent.listFiles().orEmpty())
        return CustomGameCoverRepository(context).findCustomCoverPath(path)
            ?: CoverArtRepository(context).findCachedCoverPath(serial, title = title)
            ?: PspGameMetadataReader.extractIcon0(context, path)
            ?: coverCandidates[baseName]?.absolutePath
            ?: coverCandidates[titleKey]?.absolutePath
    }

    private fun findDocumentCover(path: String, context: Context, serial: String?, title: String?): String? {
        CustomGameCoverRepository(context).findCustomCoverPath(path)?.let { return it }
        CoverArtRepository(context).findCachedCoverUri(serial, title = title)?.let { return it }
        PspGameMetadataReader.extractIcon0(context, path)?.let { return it }

        val uri = path.toUri()
        val document = DocumentFile.fromSingleUri(context, uri) ?: return null
        val parent = findDocumentParent(context, uri) ?: return null
        val baseName = normalizeBaseName(document.name.orEmpty().substringBeforeLast('.'))
        val titleKey = normalizeBaseName(cleanGameName(title ?: document.name.orEmpty().substringBeforeLast('.')))
        val coverCandidates = buildDocumentCoverCandidates(parent.listFiles())
        return coverCandidates[baseName]?.uri?.toString()
            ?: coverCandidates[titleKey]?.uri?.toString()
    }

    private fun findDocumentParent(context: Context, documentUri: Uri): DocumentFile? {
        val documentId = DocumentsContract.getDocumentId(documentUri)
        val parentId = parentDocumentId(documentId) ?: return null
        val persistedTrees = context.contentResolver.persistedUriPermissions
            .asSequence()
            .filter { permission -> permission.isReadPermission && permission.uri.authority == documentUri.authority }
            .mapNotNull { permission ->
                val rootId = runCatching { DocumentsContract.getTreeDocumentId(permission.uri) }.getOrNull()
                    ?: return@mapNotNull null
                Triple(permission.uri, rootId, relativeDocumentSegments(rootId, parentId))
            }
            .filter { (_, _, segments) -> segments != null }
            .sortedByDescending { (_, rootId, _) -> rootId.length }
            .toList()

        for ((treeUri, _, nullableSegments) in persistedTrees) {
            var current = DocumentFile.fromTreeUri(context, treeUri) ?: continue
            var found = true
            for (segment in nullableSegments.orEmpty()) {
                current = current.findFile(segment) ?: run {
                    found = false
                    break
                }
            }
            if (found) return current
        }

        return null
    }

    private fun normalizeBaseName(value: String): String {
        return value.lowercase()
            .replace('_', ' ')
            .replace('-', ' ')
            .replace(Regex("""\[[^]]*]|\([^)]*\)"""), " ")
            .replace(Regex("""\b(disc|disk|cd|dvd)\s*[0-9]+\b"""), " ")
            .replace(Regex("""\b\(?(usa|eur|europe|japan|jpn|beta|demo|proto|prototype|rev\s*[a-z0-9]+)\)?\b"""), " ")
            .replace(Regex("\\s+"), " ")
            .trim()
    }

    private fun cleanGameName(value: String): String {
        return value.substringBeforeLast('.')
            .replace(Regex("""\s+"""), " ")
            .trim()
    }

    private fun cleanScannedTitle(rawTitle: String, displayName: String): String {
        return EmulatorBridge.cleanGameDisplayTitle(rawTitle, displayName)
    }

    private fun buildLocalCoverCandidates(children: Array<out File>): Map<String, File> {
        val directFiles = children.filter { it.isFile && it.extension.lowercase() in COVER_EXTENSIONS }
        val nestedCoverFiles = children
            .filter { it.isDirectory && normalizeBaseName(it.name) in COVER_DIRECTORY_NAMES }
            .flatMap { it.listFiles().orEmpty().asIterable() }
            .filter { it.isFile && it.extension.lowercase() in COVER_EXTENSIONS }

        return (directFiles + nestedCoverFiles).associateBy { normalizeBaseName(cleanGameName(it.nameWithoutExtension)) }
    }

    private fun buildDocumentCoverCandidates(
        children: Array<DocumentFile>
    ): Map<String, DocumentFile> {
        val directFiles = children
            .filter {
                runCatching { it.type }.getOrNull() != DocumentsContract.Document.MIME_TYPE_DIR &&
                    documentDisplayName(null, it)
                        .substringAfterLast('.', "")
                        .lowercase() in COVER_EXTENSIONS
            }
        val nestedCoverFiles = children
            .filter {
                val type = runCatching { it.type }.getOrNull()
                (type == DocumentsContract.Document.MIME_TYPE_DIR || type == null) &&
                    normalizeBaseName(documentDisplayName(null, it)) in COVER_DIRECTORY_NAMES
            }
            .flatMap { runCatching { it.listFiles().asIterable() }.getOrDefault(emptyList()) }
            .filter {
                runCatching { it.type }.getOrNull() != DocumentsContract.Document.MIME_TYPE_DIR &&
                    documentDisplayName(null, it)
                        .substringAfterLast('.', "")
                        .lowercase() in COVER_EXTENSIONS
            }

        return (directFiles + nestedCoverFiles).associateBy {
            normalizeBaseName(cleanGameName(documentDisplayName(null, it).substringBeforeLast('.')))
        }
    }

    private fun documentDisplayName(context: Context?, document: DocumentFile): String {
        val rawName = runCatching { document.name }.getOrNull().orEmpty()
        if (rawName.isNotBlank()) return DocumentPathResolver.normalizeDisplayName(rawName)
        if (context == null) return ""
        return runCatching { DocumentPathResolver.getDisplayName(context, document.uri.toString()) }
            .getOrDefault("")
    }

    private class DocumentScanBudget {
        private var visitedEntries = 0
        private val visitedDirectories = HashSet<String>()

        fun tryEnterDirectory(uri: String): Boolean = visitedDirectories.add(uri)

        fun tryVisitEntry(): Boolean {
            if (visitedEntries >= MAX_DOCUMENT_SCAN_ENTRIES) return false
            visitedEntries++
            return true
        }
    }
}

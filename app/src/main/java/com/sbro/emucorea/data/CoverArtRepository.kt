package com.sbro.emucorea.data

import android.content.Context
import android.graphics.BitmapFactory
import android.util.Log
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.Locale
import java.util.concurrent.CountDownLatch
import com.sbro.emucorea.data.psp.PspCoverIndexRepository
import java.security.MessageDigest

data class CoverCacheClearResult(
    val deletedFiles: Int,
    val freedBytes: Long,
    val failedFiles: Int
) {
    val fullyCleared: Boolean
        get() = failedFiles == 0
}

class CoverArtRepository(context: Context) {

    companion object {
        private const val TAG = "CoverArtRepository"
        private const val CONNECT_TIMEOUT_MS = 10000
        private const val READ_TIMEOUT_MS = 15000
        private const val MISS_TTL_MS = 7L * 24L * 60L * 60L * 1000L // 7 days
        private const val CONFIRMED_MISS_PREFIX = "all-sources-404\n"

        /** Versioned cache directory; bump to discard stale downloaded covers and .miss markers. */
        const val CACHE_DIR_NAME = "igdb-psp-v15"
        private val downloadFlightsLock = Any()
        private val downloadFlights = HashMap<String, PendingCoverDownload>()
    }

    private class PendingCoverDownload {
        val completed = CountDownLatch(1)
        @Volatile var result: String? = null
    }

    private data class CoverDownloadAttempt(val path: String? = null, val notFound: Boolean = false)

    private val context = context.applicationContext

    private val coverCacheDirectory = File(this.context.cacheDir, "game-covers")
    private val remoteImageCacheDirectory = File(this.context.cacheDir, "remote-image-cache")

    private val cacheDirectory by lazy {
        File(coverCacheDirectory, CACHE_DIR_NAME).apply {
            if (!exists()) mkdirs()
            Log.d(TAG, "Cover cache directory created: $absolutePath")
        }
    }

    private fun cacheDirectoryForSource(baseUrl: String?): File {
        if (baseUrl == null) return cacheDirectory
        val hash = MessageDigest.getInstance("SHA-256").digest(baseUrl.toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it) }
        return File(cacheDirectory, "source_$hash").apply { mkdirs() }
    }

    /** Clears only automatically downloaded covers. User-selected covers live in filesDir and are preserved. */
    fun clearCache(): CoverCacheClearResult = clearDirectories(listOf(coverCacheDirectory))

    /** Clears all temporary cover/image downloads without touching library metadata or custom artwork. */
    fun clearAllTemporaryImageCaches(): CoverCacheClearResult =
        clearDirectories(listOf(coverCacheDirectory, remoteImageCacheDirectory))

    fun isManagedCoverCachePath(path: String?): Boolean =
        CoverCachePolicy.isPathInside(path, coverCacheDirectory)

    fun isMissingManagedCover(path: String?): Boolean =
        isManagedCoverCachePath(path) && !path.isNullOrBlank() && !isUsableCoverFile(File(path))

    fun hasIndexedCover(serial: String?, title: String?): Boolean =
        com.sbro.emucorea.data.psp.PspCoverIndexRepository(context).find(serial, title) != null

    fun hasCoverSource(serial: String?, title: String?, styleOverride: Int? = null): Boolean {
        val style = resolveCoverArtStyle(styleOverride)
        if (style == AppPreferences.COVER_ART_STYLE_DISABLED) return false
        return if (resolveCoverBaseUrl(style) != null) normalizeSerial(serial) != null
        else hasIndexedCover(serial, title)
    }

    fun needsCoverDownload(game: GameItem, styleOverride: Int? = null): Boolean {
        val style = resolveCoverArtStyle(styleOverride)
        if (style == AppPreferences.COVER_ART_STYLE_DISABLED ||
            CustomGameCoverRepository(context).isCustomCoverPath(game.coverArtPath)) return false
        if (hasCoverSource(game.serial, game.title, style)) {
            val cached = findCachedCoverPath(game.serial, style, title = game.title)
            return cached == null || cached != game.coverArtPath
        }
        return game.coverArtPath == null || game.coverArtPath.startsWith("http") ||
            isMissingManagedCover(game.coverArtPath) || (isManagedCoverCachePath(game.coverArtPath) &&
                !game.coverArtPath.replace('\\', '/').contains("/$CACHE_DIR_NAME/"))
    }

    /** Never flash an embedded icon or a previous style before the selected artwork loads. */
    fun displayCoverPath(serial: String?, title: String?, candidate: String?): String? {
        if (candidate != null && !isManagedCoverCachePath(candidate)) return candidate
        if (hasIndexedCover(serial, title)) return findCachedCoverPath(serial, title = title)
        return candidate
    }

    fun findCachedCoverPath(
        serial: String?,
        styleOverride: Int? = null,
        ignoreDisabled: Boolean = false,
        title: String? = null
    ): String? {
        val style = resolveCoverArtStyle(styleOverride)
        if (!ignoreDisabled && style == AppPreferences.COVER_ART_STYLE_DISABLED) {
            return null
        }
        val coverKeys = coverKeys(serial, title)
        if (coverKeys.isEmpty()) {
            Log.d(TAG, "No serial or title provided")
            return null
        }
        val directory = cacheDirectoryForSource(resolveCoverBaseUrl(style))
        val preferredFiles = coverKeys.flatMap { key ->
            if (style == AppPreferences.COVER_ART_STYLE_3D) {
                listOf(
                    File(directory, "${key}_3d.webp"),
                    File(directory, "${key}_3d.png"),
                    File(directory, "${key}_3d.jpg")
                )
            } else {
                listOf(
                    File(directory, "$key.jpg"),
                    File(directory, "$key.png")
                )
            }
        }
        preferredFiles.filter(File::exists).forEach { file ->
            if (!isUsableCoverFile(file)) {
                Log.w(TAG, "Removing invalid cached cover: ${file.absolutePath}")
                runCatching { file.delete() }
            }
        }
        val found = preferredFiles.firstOrNull(::isUsableCoverFile)
        Log.d(TAG, "Cached cover for $coverKeys (style=$style): ${if (found != null) "FOUND" else "NOT FOUND"}")
        return found?.absolutePath
    }

    fun findCachedCoverUri(
        serial: String?,
        styleOverride: Int? = null,
        ignoreDisabled: Boolean = false,
        title: String? = null
    ): String? {
        return findCachedCoverPath(serial, styleOverride, ignoreDisabled, title)
    }

    /**
     * Cache keys in lookup order: the normalized serial first, then a slug derived from the
     * title so games without a readable serial still resolve through the bundled title index.
     */
    private fun coverKeys(serial: String?, title: String?): List<String> {
        val keys = mutableListOf<String>()
        normalizeSerial(serial)?.let { keys += it }
        if (!title.isNullOrBlank()) {
            val identity = PspCoverIndexRepository.normalizeTitle(title)
            if (identity.isNotBlank()) {
                val hash = MessageDigest.getInstance("SHA-256").digest(identity.toByteArray(Charsets.UTF_8))
                    .joinToString("") { "%02x".format(it) }
                keys += "title_$hash"
            }
        }
        return keys
    }

    fun downloadCover(
        serial: String?,
        styleOverride: Int? = null,
        ignoreDisabled: Boolean = false,
        title: String? = null
    ): String? {
        val style = resolveCoverArtStyle(styleOverride)
        if (!ignoreDisabled && style == AppPreferences.COVER_ART_STYLE_DISABLED) {
            Log.d(TAG, "Cover download skipped: cover art style is disabled")
            return null
        }
        val coverBaseUrl = resolveCoverBaseUrl(style)
        val normalizedSerial = normalizeSerial(serial)
        if (normalizedSerial == null) {
            if (title.isNullOrBlank()) {
                Log.w(TAG, "Cannot download cover: no PSP serial or title")
                return null
            }
            if (coverBaseUrl != null) {
                // Custom cover servers are serial-addressed; a filename slug would only add 404 noise.
                Log.d(TAG, "Custom cover server configured but no PSP serial available")
                return null
            }
            // Title-only lookups never guess: the bundled index must map the title explicitly.
            val indexed = com.sbro.emucorea.data.psp.PspCoverIndexRepository(context)
                .find(null, title, style == AppPreferences.COVER_ART_STYLE_3D) != null
            if (!indexed) {
                Log.d(TAG, "No bundled cover index entry for title")
                return null
            }
        }
        val coverKey = coverKeys(serial, title).first()
        val flightKey = "${cacheDirectory.absolutePath}|$style|$coverBaseUrl|$coverKey"
        lateinit var flight: PendingCoverDownload
        var ownsDownload = false
        synchronized(downloadFlightsLock) {
            flight = downloadFlights[flightKey] ?: PendingCoverDownload().also {
                downloadFlights[flightKey] = it
                ownsDownload = true
            }
        }
        if (!ownsDownload) {
            try {
                flight.completed.await()
            } catch (interrupted: InterruptedException) {
                Thread.currentThread().interrupt()
                return null
            }
            return flight.result
        }
        return try {
            downloadCoverResolved(serial, coverKey, style, coverBaseUrl, title).also {
                flight.result = it
            }
        } finally {
            synchronized(downloadFlightsLock) {
                downloadFlights.remove(flightKey, flight)
            }
            flight.completed.countDown()
        }
    }

    private fun downloadCoverResolved(
        originalSerial: String?,
        normalizedSerial: String,
        style: Int,
        coverBaseUrl: String?,
        title: String?
    ): String? {
        val targetExtension = if (style == AppPreferences.COVER_ART_STYLE_3D) "webp" else "jpg"
        val directory = cacheDirectoryForSource(coverBaseUrl)

        // A previous fallback may have succeeded as PNG; do not request the missing JPG again.
        val extensions = if (style == AppPreferences.COVER_ART_STYLE_3D) listOf("webp", "png", "jpg") else listOf("jpg", "png")
        extensions.map { File(directory, cacheFileName(normalizedSerial, style, it)) }
            .firstOrNull(::isUsableCoverFile)?.let { return it.absolutePath }

        Log.d(TAG, "========== COVER DOWNLOAD START ==========")
        Log.d(TAG, "Original serial: $originalSerial")
        Log.d(TAG, "Normalized serial: $normalizedSerial")
        Log.d(TAG, "Cover base URL: $coverBaseUrl")
        Log.d(TAG, "Cover style: $style")

        val coverFile = File(directory, cacheFileName(normalizedSerial, style, targetExtension))
        if (isUsableCoverFile(coverFile)) {
            Log.d(TAG, "Cover already exists: ${coverFile.absolutePath}")
            Log.d(TAG, "========== COVER DOWNLOAD END (CACHED) ==========")
            return coverFile.absolutePath
        }
        if (coverFile.exists()) coverFile.delete()

        val missFile = File(directory, cacheMissFileName(normalizedSerial, style))
        if (missFile.exists()) {
            val confirmed = runCatching { missFile.readText().startsWith(CONFIRMED_MISS_PREFIX) }.getOrDefault(false)
            val age = System.currentTimeMillis() - missFile.lastModified()
            if (confirmed && age in 0 until MISS_TTL_MS) {
                Log.d(TAG, "Recent confirmed miss marker found, skipping (age: ${age}ms)")
                Log.d(TAG, "========== COVER DOWNLOAD END (MISS CACHED) ==========")
                return null
            }
            // Old markers could have been written before a fallback failed temporarily.
            missFile.delete()
        }

        val attempts = mutableListOf<CoverDownloadAttempt>()
        fun attempt(url: String, target: File, source: String, sha256: String? = null): String? {
            val downloaded = downloadFromUrl(url, target, source, sha256)
            attempts += downloaded
            return downloaded.path
        }

        var result: String? = null
        if (coverBaseUrl != null) {
            val extensionsToTry = if (style == AppPreferences.COVER_ART_STYLE_3D) listOf("png", "jpg") else listOf("jpg", "png")
            for (extension in extensionsToTry) {
                val targetFile = File(directory, cacheFileName(normalizedSerial, style, extension))
                result = attempt("$coverBaseUrl/$normalizedSerial.$extension", targetFile, "Custom")
                if (result != null) break
            }
        } else {
            val cover = com.sbro.emucorea.data.psp.PspCoverIndexRepository(context).find(originalSerial, title, style == AppPreferences.COVER_ART_STYLE_3D)
            if (cover != null) {
                result = attempt(cover.url, coverFile, "EmuCoreA PSP covers", cover.sha256)
                if (result == null && style != AppPreferences.COVER_ART_STYLE_3D) {
                    result = attempt(cover.sourceUrl, coverFile, "IGDB PSP catalog")
                }
            }
        }

        if (result == null && attempts.isNotEmpty() && attempts.all { it.notFound }) {
            runCatching { missFile.writeText("$CONFIRMED_MISS_PREFIX${System.currentTimeMillis()}") }
                .onFailure { Log.w(TAG, "Unable to cache confirmed cover miss", it) }
        } else {
            missFile.delete()
        }

        Log.d(TAG, "Download result: ${if (result != null) "SUCCESS" else "FAILED"}")
        Log.d(TAG, "========== COVER DOWNLOAD END ==========")
        return result
    }

    fun buildPublicCoverUrl(
        serial: String?,
        styleOverride: Int? = null,
        title: String? = null
    ): String? {
        val normalizedSerial = normalizeSerial(serial)
        val style = resolveCoverArtStyle(styleOverride)
        if (style == AppPreferences.COVER_ART_STYLE_DISABLED) return null
        val baseUrl = resolveCoverBaseUrl(style)
        if (baseUrl != null && normalizedSerial != null) return "$baseUrl/$normalizedSerial.${if (style == AppPreferences.COVER_ART_STYLE_3D) "png" else "jpg"}"
        return com.sbro.emucorea.data.psp.PspCoverIndexRepository(context)
            .find(serial, title, style == AppPreferences.COVER_ART_STYLE_3D)?.url
    }

    private fun downloadFromUrl(
        urlString: String,
        coverFile: File,
        sourceName: String,
        expectedSha256: String? = null
    ): CoverDownloadAttempt {
        if (isUsableCoverFile(coverFile)) {
            return CoverDownloadAttempt(path = coverFile.absolutePath)
        }
        if (coverFile.exists()) coverFile.delete()

        val connection = runCatching { URL(urlString).openConnection() as? HttpURLConnection }.getOrNull()
            ?: run {
                Log.e(TAG, "$sourceName: Failed to create HTTP connection")
                return CoverDownloadAttempt()
            }

        return try {
            connection.requestMethod = "GET"
            connection.instanceFollowRedirects = true
            connection.connectTimeout = CONNECT_TIMEOUT_MS
            connection.readTimeout = READ_TIMEOUT_MS
            connection.setRequestProperty("User-Agent", "Mozilla/5.0 (EmuCoreA)")
            connection.connect()

            val responseCode = connection.responseCode
            Log.d(TAG, "$sourceName: HTTP response code: $responseCode")

            if (responseCode != HttpURLConnection.HTTP_OK) {
                Log.w(TAG, "$sourceName: Cover not found (HTTP $responseCode)")
                return CoverDownloadAttempt(notFound = responseCode == HttpURLConnection.HTTP_NOT_FOUND)
            }

            val contentLength = connection.contentLength
            Log.d(TAG, "$sourceName: Content length: $contentLength bytes")

            // Chunked HTTP responses legitimately report -1. Only an explicitly empty body is invalid.
            if (contentLength == 0 || contentLength > 16 * 1024 * 1024) {
                Log.w(TAG, "$sourceName: Invalid content length")
                return CoverDownloadAttempt()
            }

            val tempFile = File.createTempFile("${coverFile.name}.", ".tmp", cacheDirectory)
            try {
                connection.inputStream.use { input ->
                    tempFile.outputStream().use { output ->
                        var copied = 0L
                        val buffer = ByteArray(8192)
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            copied += count
                            require(copied <= 16 * 1024 * 1024) { "Cover exceeds size limit" }
                            output.write(buffer, 0, count)
                        }
                        Log.d(TAG, "$sourceName: Copied $copied bytes")
                    }
                }

                if (expectedSha256 != null) {
                    val digest = java.security.MessageDigest.getInstance("SHA-256")
                    tempFile.inputStream().use { input ->
                        val buffer = ByteArray(8192)
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            digest.update(buffer, 0, count)
                        }
                    }
                    val actual = digest.digest().joinToString("") { "%02x".format(it) }
                    if (actual != expectedSha256) return CoverDownloadAttempt()
                }
                if (!isUsableCoverFile(tempFile)) {
                    Log.w(TAG, "$sourceName: Downloaded file is not a valid image")
                    return CoverDownloadAttempt()
                }

                if (coverFile.exists()) coverFile.delete()
                if (!tempFile.renameTo(coverFile)) {
                    tempFile.copyTo(coverFile, overwrite = true)
                }
                if (!isUsableCoverFile(coverFile)) {
                    coverFile.delete()
                    return CoverDownloadAttempt()
                }

                Log.d(TAG, "$sourceName: SUCCESS - ${coverFile.absolutePath}")
                CoverDownloadAttempt(path = coverFile.absolutePath)
            } finally {
                tempFile.delete()
            }
        } catch (e: Exception) {
            Log.e(TAG, "$sourceName: Error: ${e.message}", e)
            CoverDownloadAttempt()
        } finally {
            connection.disconnect()
        }
    }

    private fun normalizeSerial(serial: String?): String? {
        val compact = PspCoverIndexRepository.normalizeSerial(serial) ?: return null
        return "${compact.take(4)}-${compact.drop(4)}"
    }

    private fun resolveCoverBaseUrl(style: Int = resolveCoverArtStyle()): String? {
        val preferences = AppPreferences(context)
        val configuredUrls = preferences.getCoverDownloadBaseUrlSync()
            ?.split(Regex("\\s+"))
            ?.map { it.trim().trimEnd('/') }
            ?.filter { it.isNotBlank() }
            .orEmpty()
        if (configuredUrls.isNotEmpty()) {
            return if (style == AppPreferences.COVER_ART_STYLE_3D) {
                configuredUrls.getOrNull(1) ?: configuredUrls.first()
            } else {
                configuredUrls.first()
            }
        }
        return null
    }

    private fun resolveCoverArtStyle(styleOverride: Int? = null): Int {
        return styleOverride ?: AppPreferences(context).getCoverArtStyleSync()
    }

    private fun cacheFileName(serial: String, style: Int, extension: String): String {
        return if (style == AppPreferences.COVER_ART_STYLE_3D) {
            "${serial}_3d.$extension"
        } else {
            "$serial.$extension"
        }
    }

    private fun cacheMissFileName(serial: String, style: Int): String {
        return if (style == AppPreferences.COVER_ART_STYLE_3D) {
            "${serial}_3d.miss"
        } else {
            "$serial.miss"
        }
    }

    private fun isUsableCoverFile(file: File): Boolean {
        if (!file.isFile || file.length() <= 0L) return false
        return runCatching {
            val options = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeFile(file.absolutePath, options)
            options.outWidth > 0 && options.outHeight > 0
        }.getOrDefault(false)
    }

    private fun clearDirectories(directories: List<File>): CoverCacheClearResult {
        var deletedFiles = 0
        var freedBytes = 0L
        var failedFiles = 0

        directories.distinctBy { it.absolutePath }.forEach { directory ->
            if (!directory.exists()) return@forEach
            val root = runCatching { directory.canonicalFile }.getOrNull() ?: run {
                failedFiles++
                return@forEach
            }
            directory.walkBottomUp().forEach { entry ->
                if (entry == directory) return@forEach
                val safeEntry = runCatching { entry.canonicalFile }.getOrNull()
                if (safeEntry == null || !CoverCachePolicy.isFileInside(safeEntry, root)) {
                    failedFiles++
                    return@forEach
                }
                if (entry.isFile) {
                    val size = entry.length()
                    if (runCatching { entry.delete() }.getOrDefault(false)) {
                        deletedFiles++
                        freedBytes += size
                    } else {
                        failedFiles++
                    }
                } else if (entry.isDirectory) {
                    // Remove only now-empty cache subdirectories; the two cache roots remain available.
                    runCatching { entry.delete() }
                }
            }
        }
        return CoverCacheClearResult(deletedFiles, freedBytes, failedFiles)
    }

}

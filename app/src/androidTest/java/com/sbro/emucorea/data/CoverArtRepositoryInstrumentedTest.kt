package com.sbro.emucorea.data

import android.content.Context
import android.content.ContextWrapper
import android.graphics.Bitmap
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.InetAddress
import java.net.ServerSocket
import java.util.concurrent.CopyOnWriteArrayList
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class CoverArtRepositoryInstrumentedTest {
    private class FixtureServer : AutoCloseable {
        private val socket = ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"))
        val baseUrl = "http://127.0.0.1:${socket.localPort}"
        val requests = CopyOnWriteArrayList<String>()
        @Volatile var jpgStatus = 404
        @Volatile var pngStatus = 200
        private val png = ByteArrayOutputStream().use { output ->
            val bitmap = Bitmap.createBitmap(20, 30, Bitmap.Config.ARGB_8888)
            bitmap.eraseColor(android.graphics.Color.BLUE)
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, output)
            bitmap.recycle()
            output.toByteArray()
        }
        private val worker = Thread {
            while (!socket.isClosed) {
                try {
                    socket.accept().use { client ->
                        val reader = client.getInputStream().bufferedReader()
                        val path = reader.readLine().split(' ')[1]
                        while (!reader.readLine().isNullOrBlank()) { }
                        requests += path
                        val status = if (path.endsWith(".png")) pngStatus else jpgStatus
                        val body = if (path.endsWith(".png") && status == 200) png else "not an image".toByteArray()
                        val output = client.getOutputStream()
                        output.write("HTTP/1.1 $status Fixture\r\nContent-Length: ${body.size}\r\nConnection: close\r\n\r\n".toByteArray())
                        output.write(body)
                        output.flush()
                    }
                } catch (error: Exception) {
                    if (!socket.isClosed) throw error
                }
            }
        }.apply { isDaemon = true; start() }
        override fun close() { socket.close(); worker.join(1_000) }
    }

    private fun withRepository(test: (CoverArtRepository, File) -> Unit) {
        val target = InstrumentationRegistry.getInstrumentation().targetContext
        val root = java.nio.file.Files.createTempDirectory(target.cacheDir.toPath(), "cover-test-").toFile()
        val isolated = object : ContextWrapper(target) {
            override fun getApplicationContext(): Context = this
            override fun getCacheDir(): File = root
        }
        try { test(CoverArtRepository(isolated), root) } finally { root.deleteRecursively() }
    }

    // Exercise the real download orchestration without changing the user's source preferences.
    private fun download(repository: CoverArtRepository, baseUrl: String): String? {
        val method = CoverArtRepository::class.java.getDeclaredMethod(
            "downloadCoverResolved", String::class.java, String::class.java,
            Int::class.javaPrimitiveType, String::class.java, String::class.java
        ).apply { isAccessible = true }
        return method.invoke(repository, "ULUS99991", "ULUS-99991", 0, baseUrl, null) as String?
    }

    private fun remoteDownload(root: File, url: String): File? {
        val target = InstrumentationRegistry.getInstrumentation().targetContext
        val context = object : ContextWrapper(target) {
            override fun getApplicationContext(): Context = this
            override fun getCacheDir(): File = root
        }
        val method = Class.forName("com.sbro.emucorea.ui.common.GameCoverArtKt").getDeclaredMethod(
            "getOrCreateRemoteImageCacheFile", Context::class.java, String::class.java
        ).apply { isAccessible = true }
        return method.invoke(null, context, url) as File?
    }

    @Test fun missingJpgFallsBackToValidPngAndRemovesMissMarker() = withRepository { repository, root ->
        FixtureServer().use { server ->
            val path = download(repository, server.baseUrl)
            assertNotNull(path)
            assertTrue(path!!.endsWith(".png"))
            assertEquals(listOf("/ULUS-99991.jpg", "/ULUS-99991.png"), server.requests.toList())
            assertFalse(root.walk().any { it.name.endsWith(".miss") })
            assertEquals(path, download(repository, server.baseUrl))
            assertEquals(1, server.requests.count { it.endsWith(".png") })
            assertEquals(2, server.requests.size)
        }
    }

    @Test fun invalidPrimaryImageFallsBackToValidPng() = withRepository { repository, _ ->
        FixtureServer().use { server ->
            server.jpgStatus = 200
            assertNotNull(download(repository, server.baseUrl))
            assertEquals(listOf("/ULUS-99991.jpg", "/ULUS-99991.png"), server.requests.toList())
        }
    }

    @Test fun transientFallbackFailureDoesNotPreventRetryAfterRecovery() = withRepository { repository, root ->
        FixtureServer().use { server ->
            server.pngStatus = 503
            assertNull(download(repository, server.baseUrl))
            assertFalse(root.walk().any { it.name.endsWith(".miss") })
            server.pngStatus = 200
            assertNotNull(download(repository, server.baseUrl))
            assertEquals(4, server.requests.size)
        }
    }

    @Test fun transientPrimaryFailureIsNotHiddenByMissingFallback() = withRepository { repository, root ->
        FixtureServer().use { server ->
            server.jpgStatus = 503
            server.pngStatus = 404
            assertNull(download(repository, server.baseUrl))
            assertFalse(root.walk().any { it.name.endsWith(".miss") })
            server.pngStatus = 200
            assertNotNull(download(repository, server.baseUrl))
        }
    }

    @Test fun bothMissingSourcesAreCachedUntilTheMarkerExpires() = withRepository { repository, root ->
        FixtureServer().use { server ->
            server.pngStatus = 404
            assertNull(download(repository, server.baseUrl))
            val marker = root.walk().single { it.name.endsWith(".miss") }
            assertNull(download(repository, server.baseUrl))
            assertEquals(2, server.requests.size)
            assertTrue(marker.setLastModified(0))
            server.pngStatus = 200
            assertNotNull(download(repository, server.baseUrl))
            assertFalse(marker.exists())
        }
    }

    @Test fun legacyMissMarkerCannotKeepARecoveredCoverBlocked() = withRepository { repository, root ->
        FixtureServer().use { server ->
            val directory = CoverArtRepository::class.java.getDeclaredMethod("cacheDirectoryForSource", String::class.java)
                .apply { isAccessible = true }
            val cache = directory.invoke(repository, server.baseUrl) as File
            val marker = File(cache, "ULUS-99991.miss").apply { writeText("404 at ${System.currentTimeMillis()}") }
            assertNotNull(download(repository, server.baseUrl))
            assertFalse(marker.exists())
        }
    }

    @Test fun incorrectChecksumRejectsImageWithoutLeavingPartialFiles() = withRepository { repository, root ->
        FixtureServer().use { server ->
            val cache = File(root, "game-covers/${CoverArtRepository.CACHE_DIR_NAME}").apply { mkdirs() }
            val target = File(cache, "checksum.png")
            val method = CoverArtRepository::class.java.getDeclaredMethod(
                "downloadFromUrl", String::class.java, File::class.java, String::class.java, String::class.java
            ).apply { isAccessible = true }
            method.invoke(repository, "${server.baseUrl}/image.png", target, "fixture", "0".repeat(64))
            assertFalse(target.exists())
            assertFalse(cache.walk().any { it.name.endsWith(".tmp") || it.name.endsWith(".miss") })
        }
    }

    @Test fun clearingAutomaticCachePreservesCustomArtwork() = withRepository { repository, root ->
        FixtureServer().use { server ->
            val downloaded = File(download(repository, server.baseUrl)!!)
            val custom = File(root, "files/library/custom-game-covers/custom.png")
            custom.parentFile!!.mkdirs()
            downloaded.copyTo(custom)
            val result = repository.clearCache()
            assertTrue(result.fullyCleared)
            assertFalse(downloaded.exists())
            assertTrue(custom.exists())
            assertTrue(custom.length() > 0)
        }
    }

    @Test fun remoteFallbackRetriesAfterATemporaryServerFailure() = withRepository { _, root ->
        FixtureServer().use { server ->
            server.pngStatus = 503
            val url = "${server.baseUrl}/image.jpg"
            assertNull(remoteDownload(root, url))
            server.pngStatus = 200
            assertNotNull(remoteDownload(root, url))
        }
    }

    @Test fun remotePrimaryFailureIsNotHiddenByMissingFallback() = withRepository { _, root ->
        FixtureServer().use { server ->
            server.jpgStatus = 503
            server.pngStatus = 404
            val url = "${server.baseUrl}/image.jpg"
            assertNull(remoteDownload(root, url))
            server.pngStatus = 200
            assertNotNull(remoteDownload(root, url))
        }
    }

    @Test fun remoteConfirmedMissAvoidsRepeatedRequests() = withRepository { _, root ->
        FixtureServer().use { server ->
            server.pngStatus = 404
            val url = "${server.baseUrl}/image.jpg"
            assertNull(remoteDownload(root, url))
            assertNull(remoteDownload(root, url))
            assertEquals(2, server.requests.size)
        }
    }

    @Test fun remoteLegacyMissDoesNotBlockARecoveredImage() = withRepository { _, root ->
        FixtureServer().use { server ->
            val url = "${server.baseUrl}/image.jpg"
            val key = java.security.MessageDigest.getInstance("SHA-1").digest(url.toByteArray())
                .joinToString("") { "%02x".format(it) }
            val cache = File(root, "remote-image-cache").apply { mkdirs() }
            File(cache, "$key.miss").writeText(System.currentTimeMillis().toString())
            assertNotNull(remoteDownload(root, url))
        }
    }

    @Test fun remoteMissingJpgFallsBackToPng() = withRepository { _, root ->
        FixtureServer().use { server ->
            val image = remoteDownload(root, "${server.baseUrl}/image.jpg")
            assertNotNull(image)
            assertTrue(image!!.isFile)
            assertFalse(root.walk().any { it.name.endsWith(".miss") || it.name.endsWith(".tmp") })
        }
    }

    @Test fun confirmedRemoteMissDoesNotBypassCacheThroughRawNetworkLoad() = withRepository { _, root ->
        FixtureServer().use { server ->
            server.pngStatus = 404
            val target = InstrumentationRegistry.getInstrumentation().targetContext
            val context = object : ContextWrapper(target) {
                override fun getCacheDir(): File = root
            }
            val load = Class.forName("com.sbro.emucorea.ui.common.GameCoverArtKt").getDeclaredMethod(
                "loadBitmap", Context::class.java, String::class.java, Int::class.javaPrimitiveType, Int::class.javaPrimitiveType
            ).apply { isAccessible = true }
            assertNull(load.invoke(null, context, "${server.baseUrl}/image.jpg", 100, 150))
            assertEquals(2, server.requests.size)
        }
    }

    @Test fun invalidSerialsAndNonLatinTitlesCannotShareACacheKey() = withRepository { repository, _ ->
        val keys = CoverArtRepository::class.java.getDeclaredMethod("coverKeys", String::class.java, String::class.java)
            .apply { isAccessible = true }
        assertNotEquals(keys.invoke(repository, "UNKNOWN", "日本語"), keys.invoke(repository, "UNKNOWN", "別のゲーム"))
        @Suppress("UNCHECKED_CAST")
        val first = keys.invoke(repository, "UNKNOWN", "日本語") as List<String>
        assertFalse(first.contains("UNKNOWN"))
        assertFalse(first.contains("_"))
    }

    @Test fun missingSerialCanUseAReviewedDiscTitle() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val index = com.sbro.emucorea.data.psp.PspCoverIndexRepository(context)
        assertNotNull(index.find(null, "Eiyuu Densetsu: Sora no Kiseki SC (Disc 2)"))
    }

    @Test fun embeddedIconDoesNotBlockRetryingAnIndexedCover() = withRepository { repository, root ->
        val icon = File(root, "icon.png").apply { writeText("embedded icon") }
        val game = GameItem(title = "God of War: Chains of Olympus", path = "/games/gow.iso", fileName = "gow.iso", fileSize = 1, lastModified = 0, coverArtPath = icon.absolutePath)
        assertTrue(repository.needsCoverDownload(game, 0))
    }

    @Test fun disabledCoversAndCustomArtworkDoNotTriggerDownloads() = withRepository { repository, _ ->
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val custom = File(context.filesDir, "library/custom-game-covers/custom.png")
        val game = GameItem(title = "Tekken 6", path = "/games/tekken.iso", fileName = "tekken.iso", fileSize = 1, lastModified = 0, coverArtPath = custom.absolutePath)
        assertFalse(repository.needsCoverDownload(game, 0))
        assertFalse(repository.needsCoverDownload(game.copy(coverArtPath = null), AppPreferences.COVER_ART_STYLE_DISABLED))
    }

    @Test fun changingCustomSourceCannotReuseAnotherServersMiss() = withRepository { repository, _ ->
        FixtureServer().use { missing ->
            missing.pngStatus = 404
            assertNull(download(repository, missing.baseUrl))
            FixtureServer().use { recovered -> assertNotNull(download(repository, recovered.baseUrl)) }
        }
    }
}

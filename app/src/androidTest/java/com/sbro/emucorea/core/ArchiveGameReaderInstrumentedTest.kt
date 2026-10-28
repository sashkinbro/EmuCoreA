package com.sbro.emucorea.core

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.sbro.emucorea.data.GameRepository
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class ArchiveGameReaderInstrumentedTest {
    private val instrumentation get() = InstrumentationRegistry.getInstrumentation()
    private val context get() = instrumentation.targetContext

    private fun copyFixture(name: String, directory: File): File = File(directory, name).apply {
        instrumentation.context.assets.open("archive-fixtures/$name").use { input -> outputStream().use(input::copyTo) }
    }

    @Test fun archivesListAndExtractEachGameWithoutTouchingSource() {
        for (fixture in listOf("games.zip", "games.7z", "games-rar5.rar")) {
            val directory = File(context.cacheDir, "archive-test-$fixture").apply { mkdirs() }
            try {
                val source = copyFixture(fixture, directory)
                val original = source.readBytes()
                val members = ArchiveGameReader.list(context, source.absolutePath)
                assertEquals(fixture, setOf("first.ISO", "second.cso"), members.map { it.name }.toSet())
                for (member in members) {
                    val identity = ArchiveGamePath.create(source.absolutePath, member.name, member.index)
                    val prepared = DocumentPathResolver.prepareGameLaunchPath(context, identity)
                    assertNotNull("$fixture: ${member.name}", prepared)
                    val expected = if (member.name == "first.ISO") "first image content" else "second image content"
                    assertEquals(expected, File(prepared!!).readText())
                }
                assertArrayEquals(original, source.readBytes())
            } finally { directory.deleteRecursively() }
        }
    }


    @Test fun cacheKeepsPinnedImageAndReclaimsReleasedImages() {
        val directory = File(context.cacheDir, "archive-pin-test").apply { mkdirs() }
        try {
            val sources = (1..4).map { index ->
                File(directory, "source$index.zip").also { file ->
                    instrumentation.context.assets.open("archive-fixtures/games.zip").use { input -> file.outputStream().use(input::copyTo) }
                }
            }
            fun identity(source: File): String {
                val member = ArchiveGameReader.list(context, source.absolutePath).first()
                return ArchiveGamePath.create(source.absolutePath, member.name, member.index)
            }
            val pinned = ArchiveGameReader.prepare(context, identity(sources[0]), pinForLaunch = true)!!
            ArchiveGameReader.prepare(context, identity(sources[1]))
            ArchiveGameReader.prepare(context, identity(sources[2]))
            assertTrue(File(pinned).isFile)
            ArchiveGameReader.release(pinned)
            // Make the released file older than the retained recent file.
            File(pinned).setLastModified(1L)
            ArchiveGameReader.prepare(context, identity(sources[3]))
            assertFalse(File(pinned).exists())
        } finally { directory.deleteRecursively() }
    }

    @Test fun rar4DiscEntryIsReadable() {
        val directory = File(context.cacheDir, "archive-rar4-test").apply { mkdirs() }
        try {
            val source = copyFixture("games-rar4.rar", directory)
            val member = ArchiveGameReader.list(context, source.absolutePath).single()
            assertEquals("test.iso", member.name)
            val identity = ArchiveGamePath.create(source.absolutePath, member.name, member.index)
            assertEquals("test text document\r\n", File(DocumentPathResolver.prepareGameLaunchPath(context, identity)!!).readText())
        } finally { directory.deleteRecursively() }
    }

    @Test fun nonSeekableDocumentProviderAndInvalidArchivesAreHandled() {
        val source = "content://com.sbro.emucorea.test.archive/games.zip"
        val members = ArchiveGameReader.list(context, source)
        assertEquals(setOf("first.ISO", "second.cso"), members.map { it.name }.toSet())
        val member = members.first { it.name == "first.ISO" }
        val path = ArchiveGamePath.create(source, member.name, member.index)
        assertEquals("first image content", File(DocumentPathResolver.prepareGameLaunchPath(context, path)!!).readText())
        val invalid = File(context.cacheDir, "broken-game.zip").apply { writeText("not an archive") }
        try {
            assertTrue(ArchiveGameReader.list(context, invalid.absolutePath).isEmpty())
            assertNull(DocumentPathResolver.prepareGameLaunchPath(context, invalid.absolutePath))
        } finally { invalid.delete() }
    }

    @Test fun libraryReadsGameMetadataFromInsideArchive() {
        val directory = File(context.cacheDir, "archive-metadata-test").apply { mkdirs() }
        try {
            copyFixture("metadata.zip", directory)
            val game = GameRepository().scanDirectory(directory.absolutePath, context).single()
            assertEquals("Archive Test Game", game.title)
            assertEquals("ULUS12345", game.serial)
            assertNotNull(game.coverArtPath)
            assertTrue(File(game.coverArtPath!!).isFile)
        } finally { directory.deleteRecursively() }
    }

    @Test fun libraryShowsSeparateGamesAndPreservesExistingZipIdentity() {
        val directory = File(context.cacheDir, "archive-library-test").apply { mkdirs() }
        try {
            val zip = copyFixture("games.zip", directory)
            File(directory, "ordinary.iso").writeText("ordinary")
            val games = GameRepository().scanDirectory(directory.absolutePath, context)
            assertEquals(3, games.size)
            val archived = games.filter { it.fileName == "first.ISO" || it.fileName == "second.cso" }
            assertEquals(2, archived.size)
            assertTrue(archived.any { it.path == zip.absolutePath })
            assertTrue(archived.any { ArchiveGamePath.parse(it.path) != null })
            assertEquals(File(directory, "ordinary.iso").absolutePath,
                DocumentPathResolver.prepareGameLaunchPath(context, File(directory, "ordinary.iso").absolutePath))
            assertNotNull(DocumentPathResolver.prepareGameLaunchPath(context, zip.absolutePath))
        } finally { directory.deleteRecursively() }
    }
}

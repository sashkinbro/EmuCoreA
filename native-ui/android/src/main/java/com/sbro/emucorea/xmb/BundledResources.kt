package com.sbro.emucorea.xmb

import android.content.Context
import java.io.BufferedInputStream
import java.io.ByteArrayInputStream
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.nio.channels.FileChannel
import java.nio.file.StandardOpenOption
import java.security.MessageDigest
import java.util.zip.ZipInputStream

/**
 * Installs the bundled `native-ui.zip` once, into an app-private directory
 * named after the pack's SHA-256.
 *
 * The pack is never read from the APK at runtime: it is copied, verified by
 * hash, fully extracted into a staging directory and then renamed into place.
 * A crash or a full disk therefore leaves the previous complete pack usable,
 * and the next launch removes the staging directory and retries. Installs are
 * serialized in-process and with a file lock for the multi-process case.
 *
 * Only regular files are accepted. Absolute paths, `..`, excessive entry
 * counts and an over-sized expansion are rejected before anything is written
 * outside the staging directory.
 */
object BundledResources {
    private const val ASSET_NAME = "native-ui.zip"
    private const val ROOT_NAME = "native-ui"
    private const val COMPLETE_NAME = ".complete"
    private const val MAX_ZIP_BYTES = 16 * 1024 * 1024
    private const val MAX_UNPACKED_BYTES = 64L * 1024 * 1024
    private const val MAX_ENTRIES = 4096
    private val VERSION_NAME = Regex("^[0-9a-f]{64}$")

    data class Installed(
        val directory: File,
        val sha256: String,
        val files: Int,
        val unpackedBytes: Long,
    )

    @Volatile
    private var cached: File? = null

    /** The current pack directory, installing it first when necessary. */
    fun ensure(context: Context): File = synchronized(this) {
        cached?.takeIf { it.isDirectory }?.let { return it }
        val app = context.applicationContext
        val root = File(app.noBackupFilesDir, ROOT_NAME)
        if (!root.isDirectory && !root.mkdirs() && !root.isDirectory) {
            throw IllegalStateException("cannot create ${root.absolutePath}")
        }
        val lockFile = File(root, ".install.lock")
        FileChannel.open(
            lockFile.toPath(),
            StandardOpenOption.CREATE,
            StandardOpenOption.WRITE,
        ).use { channel ->
            channel.lock().use {
                val installed = installLocked(app, root)
                cached = installed.directory
                return installed.directory
            }
        }
    }

    /** Metadata of the pack already installed for this build, if any. */
    fun current(context: Context): Installed? {
        val root = File(context.applicationContext.noBackupFilesDir, ROOT_NAME)
        val current = File(root, "current")
        if (!current.isFile) return null
        val sha = current.readText().trim()
        if (!VERSION_NAME.matches(sha)) return null
        val directory = File(root, sha)
        return readComplete(directory) ?: return null
    }

    /** Whether the APK this code was built from actually carries a pack. */
    fun hasBundledPack(context: Context): Boolean = try {
        context.applicationContext.assets.open(ASSET_NAME).use { true }
    } catch (_: IOException) {
        false
    }

    private fun installLocked(context: Context, root: File): Installed {
        val bytes = try {
            context.assets.open(ASSET_NAME).use { it.readBytes() }
        } catch (error: IOException) {
            throw IllegalStateException(
                "native-ui resource pack is not packaged in this build; run native-ui/tools/pack_resources.py",
                error,
            )
        }
        require(bytes.size <= MAX_ZIP_BYTES) { "resource pack is unexpectedly large" }
        val sha = sha256(bytes)
        val target = File(root, sha)
        readComplete(target)?.let {
            writeCurrent(root, sha)
            cleanObsolete(root, sha)
            return it
        }

        val staging = File(root, ".staging")
        if (staging.exists() && !staging.deleteRecursively()) {
            throw IllegalStateException("cannot clear ${staging.absolutePath}")
        }
        if (!staging.mkdirs()) {
            throw IllegalStateException("cannot create ${staging.absolutePath}")
        }
        var files = 0
        var unpacked = 0L
        ZipInputStream(BufferedInputStream(ByteArrayInputStream(bytes))).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                val name = entry.name.replace('\\', '/')
                require(name.isNotEmpty() && !entry.isDirectory) { "resource pack contains a directory entry" }
                require(!name.startsWith("/")) { "absolute path in resource pack: $name" }
                require(name.split('/').none { it.isEmpty() || it == ".." }) {
                    "escaping path in resource pack: $name"
                }
                require(++files <= MAX_ENTRIES) { "resource pack has too many entries" }
                val destination = File(staging, name)
                val stagingRoot = staging.canonicalFile.toPath()
                require(destination.canonicalFile.toPath().startsWith(stagingRoot)) {
                    "escaping path in resource pack: $name"
                }
                destination.parentFile?.let { parent ->
                    if (!parent.isDirectory && !parent.mkdirs() && !parent.isDirectory) {
                        throw IllegalStateException("cannot create ${parent.absolutePath}")
                    }
                }
                FileOutputStream(destination).use { output ->
                    val buffer = ByteArray(64 * 1024)
                    var written = 0L
                    while (true) {
                        val count = zip.read(buffer)
                        if (count < 0) break
                        output.write(buffer, 0, count)
                        written += count
                        unpacked += count
                        require(unpacked <= MAX_UNPACKED_BYTES) { "resource pack expands too far" }
                    }
                }
            }
        }
        require(files > 0) { "resource pack is empty" }
        File(staging, COMPLETE_NAME).writeText("$sha\n$files\n$unpacked\n")
        if (target.exists() && !target.deleteRecursively()) {
            throw IllegalStateException("cannot replace ${target.absolutePath}")
        }
        if (!staging.renameTo(target)) {
            throw IllegalStateException("cannot move the staged pack into ${target.absolutePath}")
        }
        writeCurrent(root, sha)
        cleanObsolete(root, sha)
        return Installed(target, sha, files, unpacked)
    }

    private fun readComplete(directory: File): Installed? {
        if (!directory.isDirectory) return null
        val marker = File(directory, COMPLETE_NAME)
        if (!marker.isFile) return null
        val lines = runCatching { marker.readLines() }.getOrNull() ?: return null
        if (lines.size < 3) return null
        val sha = lines[0].trim()
        if (!VERSION_NAME.matches(sha) || directory.name != sha) return null
        val files = lines[1].trim().toIntOrNull() ?: return null
        val unpacked = lines[2].trim().toLongOrNull() ?: return null
        return Installed(directory, sha, files, unpacked)
    }

    private fun writeCurrent(root: File, sha: String) {
        File(root, "current").writeText(sha + "\n")
    }

    private fun cleanObsolete(root: File, keep: String) {
        root.listFiles()?.forEach { child ->
            if (child.isDirectory && child.name != keep && child.name != ".staging" &&
                VERSION_NAME.matches(child.name)
            ) {
                child.deleteRecursively()
            }
        }
    }

    private fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
}

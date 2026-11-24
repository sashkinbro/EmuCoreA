// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

package com.sbro.emucorea.xmb

import android.content.Context
import java.io.File
import java.nio.channels.FileChannel
import java.nio.file.StandardOpenOption
import java.security.MessageDigest

/**
 * The XMB resource tree, built on the device from the user's own firmware.
 *
 * The application carries no firmware-derived files. The host unpacks the
 * user's firmware updater (from a game disc they own, or an official updater
 * they supplied) into a temporary flash0 directory and hands it to [install],
 * which converts the shell's RCO containers, fonts and theme files into the
 * ready tree the renderer reads. The firmware itself is not kept.
 *
 * The tree is built in a staging directory, marked complete and only then
 * renamed into `noBackupFilesDir/native-ui/<id>/`, so an interrupted install
 * never looks installed. Installs are serialized in-process and with a file
 * lock.
 */
object XmbResources {
    private const val ROOT_NAME = "native-ui"
    private const val COMPLETE_NAME = ".complete"
    private const val STAGING_NAME = ".staging"
    private const val MARKER_FORMAT = "xmb-resources-v2"
    private val ID_NAME = Regex("^[0-9a-f]{64}$")

    /** The firmware's menu images don't match what the renderer needs (another version). */
    class UnsupportedFirmwareException(val firmware: String, message: String) : IllegalStateException(message)

    /** Thrown by [ensure] while no firmware has been installed. */
    class MissingException : IllegalStateException(
        "XMB resources are not installed; the user's firmware has to be installed first",
    )

    data class Installed(
        val directory: File,
        val id: String,
        /** Firmware version the tree was built from, e.g. "6.60". */
        val firmware: String,
        val files: Int,
    )

    @Volatile
    private var cached: Installed? = null

    /** The installed tree, or null when the user has not installed a firmware yet. */
    fun installed(context: Context): Installed? = synchronized(this) {
        cached?.takeIf { it.directory.isDirectory }?.let { return it }
        val root = root(context)
        val current = File(root, "current")
        val id = runCatching { current.readText().trim() }.getOrNull()
        val found = id?.takeIf { ID_NAME.matches(it) }?.let { readComplete(File(root, it)) }
        if (found == null) {
            // Nothing usable: drop whatever an older build left behind, including the
            // pack that earlier versions installed from the APK.
            cleanObsolete(root, keep = null)
        }
        cached = found
        found
    }

    fun isInstalled(context: Context): Boolean = installed(context) != null

    /** The installed directory. Throws [MissingException] when nothing is installed. */
    fun ensure(context: Context): File = installed(context)?.directory ?: throw MissingException()

    /**
     * Builds the tree from an unpacked flash0 directory (one holding
     * `vsh/resource/` and `font/`). Blocking; call off the main thread.
     */
    fun install(context: Context, flash0: File): Installed = synchronized(this) {
        require(File(flash0, "vsh/resource").isDirectory) {
            "no vsh/resource under ${flash0.absolutePath}"
        }
        val root = root(context)
        FileChannel.open(
            File(root, ".install.lock").toPath(),
            StandardOpenOption.CREATE,
            StandardOpenOption.WRITE,
        ).use { channel ->
            channel.lock().use {
                val staging = File(root, STAGING_NAME)
                if (staging.exists() && !staging.deleteRecursively()) {
                    throw IllegalStateException("cannot clear ${staging.absolutePath}")
                }
                if (!staging.mkdirs()) {
                    throw IllegalStateException("cannot create ${staging.absolutePath}")
                }
                val error = XmbNative.nativeExtractResources(flash0.absolutePath, staging.absolutePath)
                if (error.isNotEmpty()) {
                    staging.deleteRecursively()
                    throw IllegalStateException(error)
                }
                val manifest = File(staging, "manifest.json")
                if (!manifest.isFile) {
                    staging.deleteRecursively()
                    throw IllegalStateException("extractor wrote no manifest")
                }
                val manifestText = manifest.readText()
                val firmware = FIRMWARE_FIELD.find(manifestText)?.groupValues?.get(1)
                    ?.substringBefore(':')?.trim().orEmpty().ifEmpty { "unknown" }
                val unresolved = alignImages(staging, manifestText)
                if (unresolved.isNotEmpty()) {
                    staging.deleteRecursively()
                    throw UnsupportedFirmwareException(
                        firmware,
                        "firmware $firmware is not supported: ${unresolved.size} menu images are missing " +
                            "(${unresolved.take(3).joinToString()}). Use a 6.60 or 6.61 update.",
                    )
                }
                val id = sha256(manifestText.toByteArray())
                val files = staging.walkTopDown().count { it.isFile }
                File(staging, COMPLETE_NAME).writeText("$MARKER_FORMAT\n$id\n$firmware\n$files\n")
                val target = File(root, id)
                if (target.exists() && !target.deleteRecursively()) {
                    throw IllegalStateException("cannot replace ${target.absolutePath}")
                }
                if (!staging.renameTo(target)) {
                    throw IllegalStateException("cannot move the resources into ${target.absolutePath}")
                }
                File(root, "current").writeText(id + "\n")
                cleanObsolete(root, keep = id)
                val installed = Installed(target, id, firmware, files)
                cached = installed
                installed
            }
        }
    }

    /** Removes the installed tree; the XMB asks for a firmware again afterwards. */
    fun remove(context: Context) = synchronized(this) {
        cached = null
        root(context).deleteRecursively()
        Unit
    }

    private fun root(context: Context): File {
        val root = File(context.applicationContext.noBackupFilesDir, ROOT_NAME)
        if (!root.isDirectory && !root.mkdirs() && !root.isDirectory) {
            throw IllegalStateException("cannot create ${root.absolutePath}")
        }
        return root
    }

    private fun readComplete(directory: File): Installed? {
        if (!directory.isDirectory) return null
        val lines = runCatching { File(directory, COMPLETE_NAME).readLines() }.getOrNull() ?: return null
        if (lines.size < 4 || lines[0].trim() != MARKER_FORMAT) return null
        val id = lines[1].trim()
        if (id != directory.name) return null
        val files = lines[3].trim().toIntOrNull() ?: return null
        return Installed(directory, id, lines[2].trim(), files)
    }

    private fun cleanObsolete(root: File, keep: String?) {
        root.listFiles()?.forEach { child ->
            if (child.isDirectory && child.name != keep && child.name != STAGING_NAME &&
                ID_NAME.matches(child.name)
            ) {
                child.deleteRecursively()
            }
        }
        if (keep == null) File(root, "current").delete()
    }

    /**
     * Puts every image the renderer loads at the path it expects, matching by
     * the digest of the image's source payload ([XmbExpectedImages]). Labels
     * like `topmenu_icon/EV` are positional and shift between firmware
     * versions; without this an icon would land on its neighbour's item.
     * Returns the expected paths no extracted image matched.
     */
    private fun alignImages(staging: File, manifestText: String): List<String> {
        val entries = org.json.JSONObject(manifestText).getJSONArray("files")
        val pathBySha = HashMap<String, String>()
        val shaByPath = HashMap<String, String>()
        for (i in 0 until entries.length()) {
            val entry = entries.getJSONObject(i)
            val file = entry.optString("file").replace('\\', '/')
            val sha = entry.optString("sha256_source")
            if (file.isEmpty() || sha.isEmpty() || sha.startsWith("[")) continue
            shaByPath[file] = sha
            pathBySha.putIfAbsent(sha, file)
        }
        // Read every source first, so a move never overwrites an image another one still needs.
        val plan = LinkedHashMap<String, ByteArray>()
        val unresolved = ArrayList<String>()
        for ((path, expected) in XmbExpectedImages.bySourceSha256) {
            if (shaByPath[path] == expected) continue
            val source = pathBySha[expected]
            if (source == null) {
                unresolved += path
                continue
            }
            plan[path] = File(staging, source).readBytes()
        }
        for ((path, bytes) in plan) {
            val target = File(staging, path)
            target.parentFile?.mkdirs()
            target.writeBytes(bytes)
        }
        if (plan.isNotEmpty()) {
            android.util.Log.i("XmbResources", "moved ${plan.size} images to their renderer paths")
        }
        return unresolved
    }

    private val FIRMWARE_FIELD =Regex("\"firmware\"\\s*:\\s*\"([^\"]*)\"")

    private fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
}

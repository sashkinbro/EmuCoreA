// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

package com.sbro.emucorea.ui.xmb

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.util.Log
import com.sbro.emucorea.core.ArchiveGamePath
import com.sbro.emucorea.core.DocumentPathResolver
import com.sbro.emucorea.core.NativePpsspp
import com.sbro.emucorea.data.GameItem
import com.sbro.emucorea.xmb.XmbResources
import java.io.File
import java.util.function.IntConsumer

/**
 * Builds the XMB resources from a firmware the user already has.
 *
 * Nothing firmware-derived ships with the app. Most UMDs carry a firmware
 * updater (`PSP_GAME/SYSDIR/UPDATE/DATA.BIN`), so the library is checked first:
 * reading an updater's version only touches its PARAM.SFO, which keeps the scan
 * quick. When no game has one, the user picks an updater `EBOOT.PBP`, a bare
 * `DATA.BIN` or a disc image themselves.
 *
 * The updater is unpacked by the core's own PSAR unpacker into a temporary
 * directory, converted into the XMB resource tree and then deleted. No network
 * access is involved.
 */
object XmbFirmwareInstaller {
    private const val TAG = "XmbFirmware"
    private const val WORK_PREFIX = "xmb-firmware"

    /** Sources whose install failed in this process; a rescan does not pick them again. */
    private val failedLocations = java.util.Collections.newSetFromMap(java.util.concurrent.ConcurrentHashMap<String, Boolean>())

    /** One install at a time, process-wide; a second caller waits and reuses the result. */
    private val installLock = Any()

    /** Images that are worth asking the core about. Archives would need unpacking first. */
    private val DISC_EXTENSIONS = setOf("iso", "cso", "chd")

    /** A firmware source: a library game or a file the user picked. */
    data class Source(val location: String, val displayName: String, val version: String)

    /** Versions the extraction is verified against come first, then the newest. */
    private fun rank(version: String): Int {
        val number = XmbResources.versionToInt(version)
        return if (preferred(version)) 10_000 + number else number
    }

    /** Whether the version is one the XMB can be built from (6.00 or newer). */
    private fun usable(version: String) = XmbResources.versionToInt(version) >= XmbResources.MIN_FIRMWARE

    private fun preferred(version: String) = version == "6.60" || version == "6.61"

    /**
     * Checks the library for discs that carry an updater, best first: a
     * 6.60/6.61 updater ends the scan at once (it is the one the extraction is
     * verified against), otherwise every disc is checked and the newest comes
     * first. Discs whose install already failed in this process are skipped.
     */
    fun scanLibrary(context: Context, games: List<GameItem>, onProgress: (Int, Int) -> Unit): List<Source> {
        if (!NativePpsspp.ensureLoaded()) return emptyList()
        val candidates = games.filter { game ->
            !game.isDlc && game.path !in failedLocations && ArchiveGamePath.parse(game.path) == null &&
                game.fileName.substringAfterLast('.', "").lowercase() in DISC_EXTENSIONS
        }
        val found = ArrayList<Source>()
        for ((index, game) in candidates.withIndex()) {
            onProgress(index + 1, candidates.size)
            val version = runCatching {
                withDescriptor(context, game.path) { fd, hint -> NativePpsspp.nativeReadUpdaterVersionFd(fd, hint) }
            }.onFailure { Log.w(TAG, "cannot check ${game.fileName}", it) }.getOrNull().orEmpty()
            if (version.isEmpty()) continue
            if (!usable(version)) {
                Log.i(TAG, "${game.fileName}: updater $version is older than 6.00, skipped")
                continue
            }
            found += Source(game.path, game.title, version)
            if (preferred(version)) break
        }
        return found.sortedByDescending { rank(it.version) }
    }

    /** The updater version in a file the user picked, or null if it holds none. */
    fun probe(context: Context, location: String): Source? {
        if (!NativePpsspp.ensureLoaded()) return null
        return runCatching {
            withDescriptor(context, location) { fd, hint ->
                val version = NativePpsspp.nativeReadUpdaterVersionFd(fd, hint)
                version.takeIf { it.isNotEmpty() }?.let { Source(location, hint, it) }
            }
        }.onFailure { Log.w(TAG, "cannot check $location", it) }.getOrNull()
    }

    /**
     * Unpacks [source] and builds the XMB resources from it. Progress is 0..100
     * over both phases. Blocking; call off the main thread. Throws on failure.
     */
    fun install(
        context: Context,
        source: Source,
        upgrade: Boolean = false,
        onProgress: (Int) -> Unit,
    ): XmbResources.Installed =
        synchronized(installLock) {
            // A screen that was recreated mid-install waits here and finds it done.
            if (!upgrade) XmbResources.installed(context)?.let { onProgress(100); return it }
            if (!usable(source.version)) {
                throw XmbResources.UnsupportedFirmwareException(
                    source.version, "firmware ${source.version} is older than 6.00",
                )
            }
            check(NativePpsspp.ensureLoaded()) { "the native core is unavailable" }
            context.cacheDir.listFiles { file -> file.name.startsWith(WORK_PREFIX) }
                ?.forEach { it.deleteRecursively() }
            val work = File(context.cacheDir, "$WORK_PREFIX-${System.nanoTime()}")
            check(work.mkdirs()) { "cannot create ${work.absolutePath}" }
            try {
                val error = withDescriptor(context, source.location) { fd, hint ->
                    NativePpsspp.nativeUnpackUpdaterFd(
                        fd, hint, work.absolutePath, "flash0:/",
                        IntConsumer { percent -> onProgress((percent.coerceIn(0, 100) * 85) / 100) },
                    )
                }
                check(error.isEmpty()) { error }
                val flash0 = findFlash0(work) ?: error("the updater holds no XMB resources")
                onProgress(90)
                val installed = XmbResources.install(context, flash0, keepPrevious = upgrade)
                onProgress(100)
                Log.i(TAG, "XMB resources built from ${source.version} (${installed.files} files)")
                installed
            } catch (error: Throwable) {
                failedLocations += source.location
                Log.e(TAG, "install from ${source.displayName} (${source.version}) failed", error)
                throw error
            } finally {
                work.deleteRecursively()
            }
        }

    @Volatile
    private var upgradeChecked = false

    /**
     * When the installed resources came from a firmware other than 6.60/6.61,
     * looks for a better one in the library and installs it in the background.
     * Runs at most once per process. The running XMB keeps its current tree; the
     * new one is used the next time the mode opens, and the old one is removed on
     * the next start. Blocking; call off the main thread. Returns the new install,
     * or null when nothing better was found.
     */
    fun upgradeIfBetter(context: Context, games: List<GameItem>): XmbResources.Installed? {
        if (upgradeChecked) return null
        upgradeChecked = true
        val current = XmbResources.installed(context) ?: return null
        if (preferred(current.firmware)) return null
        val better = scanLibrary(context, games) { _, _ -> }
            .filter { rank(it.version) > rank(current.firmware) }
        if (better.isEmpty()) {
            Log.i(TAG, "XMB resources from ${current.firmware}; no better updater in the library")
            return null
        }
        // Best first; a disc whose updater turns out unusable falls through to the next one.
        for (candidate in better) {
            Log.i(TAG, "upgrading XMB resources ${current.firmware} -> ${candidate.version} from ${candidate.displayName}")
            val result = runCatching { install(context, candidate, upgrade = true) { } }
                .onFailure { Log.w(TAG, "background upgrade from ${candidate.displayName} failed", it) }
                .getOrNull()
            if (result != null) return result
        }
        return null
    }

    private fun findFlash0(work: File): File? =
        sequenceOf(File(work, "flash0"), work)
            .firstOrNull { File(it, "vsh/resource").isDirectory }
            ?: work.walkTopDown().maxDepth(3)
                .firstOrNull { it.isDirectory && File(it, "vsh/resource").isDirectory }

    private fun <T> withDescriptor(context: Context, location: String, block: (Int, String) -> T): T {
        val descriptor = if (location.startsWith("content://")) {
            context.contentResolver.openFileDescriptor(Uri.parse(location), "r")
        } else {
            ParcelFileDescriptor.open(File(location), ParcelFileDescriptor.MODE_READ_ONLY)
        } ?: error("cannot open $location")
        val hint = if (location.startsWith("content://")) {
            runCatching { DocumentPathResolver.getDisplayName(context, location) }.getOrNull()
                ?.takeIf { it.isNotBlank() } ?: DocumentPathResolver.getFallbackDisplayName(location)
        } else {
            File(location).name
        }
        return descriptor.use { block(it.fd, hint) }
    }
}

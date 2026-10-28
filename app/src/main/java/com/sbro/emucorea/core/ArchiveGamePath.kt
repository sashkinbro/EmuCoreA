package com.sbro.emucorea.core

import java.util.Base64
import java.util.Locale

/** Stable identity of a member of an archive, independent of its temporary extracted file. */
object ArchiveGamePath {
    private const val PREFIX = "archivegame:"
    data class Entry(val source: String, val name: String, val index: Int)

    fun create(source: String, name: String, index: Int): String {
        require(source.isNotBlank() && name.isNotBlank() && index >= 0)
        val encoder = Base64.getUrlEncoder().withoutPadding()
        fun encode(value: String) = encoder.encodeToString(value.toByteArray(Charsets.UTF_8))
        return "$PREFIX${encode(source)}.${encode(name)}.$index"
    }

    fun parse(path: String): Entry? {
        if (!path.startsWith(PREFIX)) return null
        return runCatching {
            val parts = path.removePrefix(PREFIX).split('.')
            require(parts.size == 3)
            val decoder = Base64.getUrlDecoder()
            fun decode(value: String) = String(decoder.decode(value), Charsets.UTF_8)
            Entry(decode(parts[0]), decode(parts[1]), parts[2].toInt()).also {
                require(it.source.isNotBlank() && it.name.isNotBlank() && it.index >= 0)
            }
        }.getOrNull()
    }

    fun isArchiveName(name: String): Boolean = name.substringAfterLast('.', "").lowercase(Locale.ROOT) in setOf("zip", "rar", "7z")
    fun isPlayableMember(name: String): Boolean = !name.endsWith('/') && !name.endsWith('\\') &&
        !name.replace('\\', '/').startsWith("__MACOSX/", ignoreCase = true) &&
        !isArchiveName(name) && PspGameFormats.isSupportedName(name.replace('\\', '/'))

    /** Mirrors the ZIP disc selection used by the previous PPSSPP loader. */
    fun legacyZipMemberIndex(names: List<String>): Int = names.indexOfFirst { original ->
        val name = original.lowercase(Locale.ROOT)
        !name.startsWith("__macosx/") && name.count { it == '/' } <= 1 &&
            name.substringAfterLast('.', "") in setOf("iso", "cso", "chd")
    }
}

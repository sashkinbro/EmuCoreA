package com.sbro.emucorea.data.psp

import android.content.Context
import org.json.JSONObject
import java.text.Normalizer
import java.util.Locale

/** A bundled, reviewed snapshot; regional serials share a canonical IGDB game. */
class PspCoverIndexRepository(private val context: Context) {
    data class Cover(val url: String, val sourceUrl: String, val sha256: String)

    fun find(serial: String?, title: String?, threeDimensional: Boolean = false): Cover? {
        val index = load() ?: return null
        val id = findGameId(serial, title)?.toString() ?: return null
        val game = index.optJSONObject("games")?.optJSONObject(id) ?: return null
        val path = game.optString(if (threeDimensional) "path_3d" else "path")
        if (!Regex("covers/(3d/)?[0-9]+\\.(jpg|png|webp)").matches(path)) return null
        return Cover("$BASE_URL/$path", game.optString("source_url"), game.optString(if (threeDimensional) "sha256_3d" else "sha256"))
    }

    /** Share the reviewed identity map with catalog details, not just artwork. */
    fun findGameId(serial: String?, title: String?): Long? {
        val index = load() ?: return null
        val normalizedSerial = normalizeSerial(serial).orEmpty()
        index.optJSONObject("serials")?.optString(normalizedSerial)?.toLongOrNull()?.let { return it }
        val officialTitle = PspTitleIndexRepository(context).titleForSerial(serial)
        for (candidate in listOfNotNull(officialTitle, title)) {
            index.optJSONObject("titles")?.optString(normalizeTitle(candidate))?.toLongOrNull()?.let { return it }
        }
        return null
    }

    private fun load(): JSONObject? = cached ?: synchronized(lock) {
        cached ?: runCatching {
            context.assets.open("catalog/psp_covers.json").bufferedReader().use { JSONObject(it.readText()) }
        }.getOrNull()?.also { cached = it }
    }

    companion object {
        private const val BASE_URL = "https://raw.githubusercontent.com/sashkinbro/EmuCoreA-Covers/d76160f027bd8e14d175fc5932a00f0e758dbdf3"
        private val lock = Any()
        @Volatile private var cached: JSONObject? = null

        internal fun normalizeSerial(value: String?): String? = value.orEmpty().uppercase(Locale.ROOT)
            .replace(Regex("[-_.\\s]"), "").takeIf { Regex("[A-Z]{4}[0-9]{5}").matches(it) }

        // Strip only known dump metadata; parentheses can also contain real subtitles.
        private val metadata = Regex("""(?i)[(\[]\s*(?:(?:disc|disk)\s*\d+(?:\s*of\s*\d+)?|USA|Europe|Japan|Asia|Korea|World|Australia|Russia|UK|United States|United Kingdom|(?:En|Fr|De|Es|It|Ja|Ko|Zh|Ru|Pl|Pt|Nl|Sv|No|Da|Fi)(?:\s*,\s*(?:En|Fr|De|Es|It|Ja|Ko|Zh|Ru|Pl|Pt|Nl|Sv|No|Da|Fi))*|Rev\s+[\d.]+|[A-Z]{4}[-_ .]?\d{5})\s*[)\]]""")

        internal fun normalizeTitle(value: String): String = Normalizer.normalize(
            value.replace(Regex("[™®©]"), "")
                .replace(Regex("(?i)\\.(iso|cso|chd|pbp|elf|zip)$"), "")
                .replace(metadata, " ").lowercase(Locale.ROOT), Normalizer.Form.NFKD)
            .replace(Regex("\\p{M}+"), "")
            .replace("ß", "ss").replace('ς', 'σ')
            .replace(Regex("[^\\p{L}\\p{N}]+"), " ")
            .trim()
    }
}

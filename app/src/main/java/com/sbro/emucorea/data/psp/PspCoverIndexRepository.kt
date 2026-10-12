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
        val normalizedSerial = serial.orEmpty().uppercase(Locale.ROOT).replace(Regex("[-_.\\s]"), "")
        val id = index.optJSONObject("serials")?.optString(normalizedSerial).orEmpty().ifBlank {
            index.optJSONObject("titles")?.optString(normalizeTitle(title.orEmpty())).orEmpty()
        }
        val game = index.optJSONObject("games")?.optJSONObject(id) ?: return null
        val path = game.optString(if (threeDimensional) "path_3d" else "path")
        if (!Regex("covers/(3d/)?[0-9]+\\.(jpg|png|webp)").matches(path)) return null
        return Cover("$BASE_URL/$path", game.optString("source_url"), game.optString(if (threeDimensional) "sha256_3d" else "sha256"))
    }

    private fun load(): JSONObject? = cached ?: synchronized(lock) {
        cached ?: runCatching {
            context.assets.open("catalog/psp_covers.json").bufferedReader().use { JSONObject(it.readText()) }
        }.getOrNull()?.also { cached = it }
    }

    companion object {
        private const val BASE_URL = "https://raw.githubusercontent.com/sashkinbro/EmuCoreA-Covers/a7c0a24a7a8082312fb13ae691a6c4b6a9efda45"
        private val lock = Any()
        @Volatile private var cached: JSONObject? = null

        internal fun normalizeTitle(value: String): String = Normalizer.normalize(value.lowercase(Locale.ROOT), Normalizer.Form.NFKD)
            .replace(Regex("\\p{M}+"), "")
            .replace(Regex("[™®©]"), "")
            .replace(Regex("[^\\p{L}\\p{N}]+"), " ")
            .trim()
    }
}

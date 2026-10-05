package com.sbro.emucorea.data

/** One pack root: a single textures.ini and every asset below its folder. */
internal data class TexturePackSection(
    val iniPath: String,
    val files: Map<String, String>
)

/**
 * A pack may ship several roots, one per game or region, each with its own
 * textures.ini. PPSSPP reads the ini that sits next to the assets of the
 * running disc, so every root is kept as an independent section.
 */
internal fun resolveTexturePackSections(paths: Set<String>): List<TexturePackSection> {
    val inis = paths.filter { it.substringAfterLast('/').equals("textures.ini", ignoreCase = true) }
    if (inis.isEmpty()) error("Expected a textures.ini per texture pack")
    return inis.sorted().map { mainIni ->
        val prefix = mainIni.substringBeforeLast('/', "").let { if (it.isEmpty()) "" else "$it/" }
        val destinations = hashSetOf<String>()
        val files = buildMap {
            paths.filter { it.startsWith(prefix) }.forEach { source ->
                val relative = if (source == mainIni) "textures.ini" else source.removePrefix(prefix)
                require(destinations.add(relative)) { "Duplicate texture pack path" }
                put(source, relative)
            }
        }
        TexturePackSection(mainIni, files)
    }
}

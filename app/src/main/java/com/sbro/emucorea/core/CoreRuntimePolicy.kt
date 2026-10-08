package com.sbro.emucorea.core

import java.io.File

/** Each restart owns its snapshot, including when an earlier restore is still pending. */
internal fun runtimeRendererSnapshot(cacheDirectory: File, pendingSnapshot: File?): File {
    val snapshot = File.createTempFile(".renderer-switch-", ".sav", cacheDirectory)
    try {
        pendingSnapshot?.copyTo(snapshot, overwrite = true)
        return snapshot
    } catch (error: Exception) {
        snapshot.delete()
        throw error
    }
}

/** Values sent to PPSSPP's alternate FPS limiter; zero means unlimited. */
internal fun runtimeFpsLimit(enabled: Boolean, target: Int): Int =
    if (!enabled) 0 else if (target > 0) target.coerceIn(20, 120) else 60

/** Resolves the same effective option before every game boot and renderer restart. */
internal fun runtimeCoreOptionValue(
    key: String,
    stored: String?,
    default: String,
    textureFilter: String,
    textureReplacement: String?
): String = stored ?: when (key) {
    "ppsspp_texture_filtering" -> textureFilter
    "ppsspp_texture_replacement" -> textureReplacement?.toBooleanStrictOrNull()
        ?.let { if (it) "enabled" else "disabled" } ?: default
    else -> default
}

internal fun runtimeStartCoreOptions(
    defaults: Map<String, String>,
    global: Map<String, String>,
    session: Map<String, String>,
    textureFilter: String,
    textureReplacement: String?
): Map<String, String> = LinkedHashMap<String, String>().apply {
    defaults.forEach { (key, default) ->
        put(key, runtimeCoreOptionValue(key, global[key], default, textureFilter, textureReplacement))
    }
    putAll(global)
    putAll(session)
}

internal fun runtimeDisplayRect(width: Int, height: Int, aspectMode: Int): FloatArray {
    if (aspectMode == 0) return floatArrayOf(0f, 0f, width.toFloat(), height.toFloat())
    val referenceWidth = when (aspectMode) { 2 -> 4f; 4 -> 10f; else -> 480f }
    val referenceHeight = when (aspectMode) { 2 -> 3f; 4 -> 7f; else -> 272f }
    val scale = minOf(width / referenceWidth, height / referenceHeight)
    val contentWidth = (referenceWidth * scale).toInt().coerceAtLeast(1)
    val contentHeight = (referenceHeight * scale).toInt().coerceAtLeast(1)
    val left = (width - contentWidth) / 2
    val top = (height - contentHeight) / 2
    return floatArrayOf(left.toFloat(), top.toFloat(), (left + contentWidth).toFloat(), (top + contentHeight).toFloat())
}

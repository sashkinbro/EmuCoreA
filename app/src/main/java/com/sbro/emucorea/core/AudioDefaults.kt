package com.sbro.emucorea.core

/** Audio defaults used by the Android frontend and sanitized before reaching the core. */
object AudioDefaults {
    const val VOLUME_DEFAULT = 100
    const val VOLUME_MIN = 0
    const val VOLUME_MAX = 100

    // Audio backend selection (Android-specific)
    // BACKEND_AAUDIO   → the native frontend's own AAudio output
    // BACKEND_OPENSLES → PPSSPP's OpenSL ES backend (android/jni/OpenSLContext.cpp)
    const val BACKEND_AAUDIO = 0
    const val BACKEND_OPENSLES = 1
    const val BACKEND_DEFAULT = BACKEND_AAUDIO

    // Mixer queue target, matching PPSSPP's extra audio buffering (80 ms).
    // Larger values absorb more frame-time jitter at the cost of audio
    // latency; the ring is sized so the target can go up to half of it.
    const val OUTPUT_LATENCY_MS_DEFAULT = 80
    const val OUTPUT_LATENCY_MS_MIN = 10
    const val OUTPUT_LATENCY_MS_MAX = 500
    // Do not request the platform's low latency path by default; the smaller
    // device buffer it produces leaves no room for frame-time spikes.
    const val MINIMAL_OUTPUT_LATENCY_DEFAULT = false

    fun coerceVolume(value: Int): Int = value.coerceIn(VOLUME_MIN, VOLUME_MAX)

    fun coerceBackend(value: Int): Int = when (value) {
        BACKEND_AAUDIO, BACKEND_OPENSLES -> value
        else -> BACKEND_DEFAULT
    }

    /** Native audio backend name for the given frontend backend index. */
    fun backendCoreName(value: Int): String = when (coerceBackend(value)) {
        BACKEND_OPENSLES -> "OpenSL ES"
        else -> "AAudio"
    }

    fun coerceOutputLatencyMs(value: Int): Int =
        value.coerceIn(OUTPUT_LATENCY_MS_MIN, OUTPUT_LATENCY_MS_MAX)
}

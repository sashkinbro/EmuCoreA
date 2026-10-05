package com.sbro.emucorea.core

enum class GpuVendor {
    ADRENO,
    MALI,
    POWERVR,
    XCLIPSE,
    UNKNOWN
}

enum class GpuTier {
    GREEN,
    YELLOW,
    RED,
    UNKNOWN
}

/** Adreno product families used by the SoC GPU catalog. */
enum class AdrenoFamily {
    A6XX,
    A7XX,
    A8XX
}

enum class GpuInfoSource {
    GL_RENDERER,
    SOC_CATALOG,
    UNKNOWN
}

data class GpuModelInfo(
    val vendor: GpuVendor,
    val displayName: String,
    val adrenoFamily: AdrenoFamily? = null
)

data class DeviceGpuSnapshot(
    val model: GpuModelInfo?,
    val tier: GpuTier,
    val renderer: String?,
    val source: GpuInfoSource
)

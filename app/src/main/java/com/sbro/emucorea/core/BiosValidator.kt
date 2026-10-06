package com.sbro.emucorea.core

object BiosValidator {

    private val fileNameHints = listOf("bios", "firmware", "flash0", "psp update")

    fun isLikelyBiosLibraryEntry(
        fileName: String,
        title: String?,
        serial: String?
    ): Boolean {
        val lowerFileName = fileName.lowercase()
        val lowerTitle = title.orEmpty().lowercase()
        val combined = "$lowerFileName $lowerTitle"
        return fileNameHints.any(combined::contains) ||
            combined.contains("playstation firmware") ||
            lowerFileName.startsWith("flash0") ||
            serial.orEmpty().lowercase().contains("firmware")
    }
}

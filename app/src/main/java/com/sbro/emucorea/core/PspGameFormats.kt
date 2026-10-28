package com.sbro.emucorea.core

import java.util.Locale

/** File candidates accepted by Core/Loaders.cpp; Archive contents are checked before launch. */
object PspGameFormats {
    val extensions = setOf("iso", "cso", "chd", "pbp", "elf", "prx", "plf", "zip", "rar", "7z")

    fun isSupportedName(name: String): Boolean {
        val fileName = name.substringAfterLast('/').lowercase(Locale.ROOT)
        return fileName.substringAfterLast('.', "") in extensions ||
            fileName == "eboot.bin" || fileName == "boot.bin"
    }
}

// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreA-Proprietary
package com.sbro.emucorea.core

import android.content.Context
import android.content.res.AssetManager
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap

/**
 * Localized text for the PPSSPP libretro options.
 *
 * PPSSPP already ships translations in assets/lang. Keeping the lookup here
 * means the option model remains stable (keys and persisted values never
 * change) while the two settings UIs can render the same option in the
 * selected language. The small alias table bridges libretro option wording
 * to the equivalent PPSSPP translation key.
 */
object PpssppCoreOptionLocalization {
    data class Text(
        val label: String,
        val choices: List<PpssppCoreOptions.Choice>,
    )

    /** Translation coverage for one resource language. Technical tokens may stay unchanged. */
    data class Coverage(
        val language: String,
        val optionCount: Int,
        val labelCount: Int,
        val labelTotal: Int,
        val choiceCount: Int,
        val choiceTotal: Int,
        val untranslatedLabels: List<String>,
        val untranslatedChoices: List<String>,
    )

    private data class Catalog(val values: Map<String, String>)

    private val cache = ConcurrentHashMap<String, Catalog>()

    private val languageFiles = mapOf(
        "ar" to "ar_AE.ini", "cs" to "cz_CZ.ini", "de" to "de_DE.ini",
        "es" to "es_ES.ini", "fa" to "fa_IR.ini", "fr" to "fr_FR.ini",
        // PPSSPP has no Hindi asset. Hindi is resolved from the explicit Hindi
        // dictionaries below so English en_US text never leaks into the UI.
        "hi" to "", "in" to "id_ID.ini", "id" to "id_ID.ini", "it" to "it_IT.ini",
        "ja" to "ja_JP.ini", "ko" to "ko_KR.ini", "pl" to "pl_PL.ini",
        "pt" to "pt_BR.ini", "ru" to "ru_RU.ini", "tr" to "tr_TR.ini",
        "uk" to "uk_UA.ini", "zh" to "zh_TW.ini", "en" to "en_US.ini",
    )

    // Values are PPSSPP's English translation keys. They are intentionally
    // keyed by the stable libretro option key, so wording changes in the
    // Kotlin model cannot accidentally localize a different option.
    private val aliases = mapOf(
        "ppsspp_ignore_bad_memory_access" to "Ignore Illegal Reads/Writes",
        "ppsspp_force_lag_sync" to "Force real clock sync (slower, less lag)",
        "ppsspp_locked_cpu_speed" to "Change CPU Clock",
        "ppsspp_memstick_size" to "Memory Stick size",
        "ppsspp_mulitsample_level" to "Antialiasing (MSAA)",
        "ppsspp_frameskip" to "Frame Skipping",
        "ppsspp_auto_frameskip" to "Auto FrameSkip",
        "ppsspp_analog_deadzone" to "Deadzone radius",
        "ppsspp_analog_sensitivity" to "Analog speed",
        "ppsspp_spline_quality" to "LowCurves",
        "ppsspp_hardware_tesselation" to "Hardware Tessellation",
        "ppsspp_texture_scaling_type" to "CPU texture upscaler (slow)",
        "ppsspp_texture_scaling_level" to "Upscale Level",
        "ppsspp_texture_deposterize" to "Deposterize",
        "ppsspp_gpu_hardware_transform" to "Hardware Transform",
        "ppsspp_texture_anisotropic_filtering" to "Anisotropic Filtering",
        "ppsspp_texture_filtering" to "Texture Filtering",
        "ppsspp_smart_2d_texture_filtering" to "Smart 2D texture filtering",
        "ppsspp_texture_replacement" to "Texture Replacement",
        "ppsspp_skip_buffer_effects" to "Skip Buffer Effects",
        "ppsspp_skip_gpu_readbacks" to "Skip GPU Readbacks",
        "ppsspp_enable_wlan" to "Enable networking",
        "ppsspp_enable_builtin_pro_ad_hoc_server" to "Enable built-in ad hoc server",
        "ppsspp_forced_first_connect" to "Forced First Connect",
        "ppsspp_upnp_use_original_port" to "UPnP use original port",
        "ppsspp_port_offset" to "Port offset",
        "ppsspp_minimum_timeout" to "Minimum Timeout",
        "ppsspp_wlan_channel" to "WLAN Channel",
    )

    /** Resolve all visible text while preserving the option's raw values. */
    fun resolve(context: Context, option: PpssppCoreOptions.Option): Text {
        val language = context.resources.configuration.locales[0].language.lowercase(Locale.ROOT)
        return resolveForLanguage(context.assets, language, option)
    }

    /** Resolve one option for an explicit language; used by the coverage audit. */
    fun resolveForLanguage(assets: AssetManager, language: String, option: PpssppCoreOptions.Option): Text {
        val normalizedLanguage = language.lowercase(Locale.ROOT)
        val catalog = cache.getOrPut(normalizedLanguage) { loadCatalog(assets, normalizedLanguage) }
        val englishLabel = option.label
        val alias = aliases[option.key] ?: englishLabel
        val labelCatalog = if (normalizedLanguage == "hi") Catalog(emptyMap()) else catalog
        val label = (if (option.key == "ppsspp_memstick_size") memoryStickSizeLabels[normalizedLanguage] else null)
            ?: specialLabels[normalizedLanguage]?.get(option.key)
            ?: labelCatalog.lookup(alias)?.takeUnless { it == option.label && !isTechnical(option.label) }
            ?: fallbackLabel(normalizedLanguage, labelCatalog, option)
        val choices = option.choices.map { choice ->
            choice.copy(label = choiceLabel(normalizedLanguage, labelCatalog, choice))
        }
        return Text(label = normalizeOptionTitle(label, normalizedLanguage), choices = choices)
    }

    private fun normalizeOptionTitle(label: String, language: String): String {
        val firstLetter = label.indexOfFirst(Char::isLetter)
        if (firstLetter < 0 || !label[firstLetter].isLowerCase()) return label
        val locale = Locale.forLanguageTag(language)
        return label.replaceRange(firstLetter, firstLetter + 1, label[firstLetter].titlecase(locale))
    }

    /**
     * Programmatic audit used by release checks. Counts every option, description,
     * and choice and reports only human text that remained identical to English.
     */
    fun coverage(assets: AssetManager, language: String): Coverage {
        val options = PpssppCoreOptions.all()
        val resolved = options.map { it to resolveForLanguage(assets, language, it) }
        val labels = resolved.filter { (option, text) -> text.label != option.label || isTechnical(option.label) }
        val choices = resolved.flatMap { (option, text) ->
            option.choices.zip(text.choices).filter { (source, localized) ->
                localized.label != source.label || isTechnical(source.label)
            }.map { (source, _) -> "${option.key}:${source.label}" }
        }
        val untranslatedLabels = resolved.filter { (option, text) ->
            text.label == option.label && !isTechnical(option.label)
        }.map { it.first.key }
        val untranslatedChoices = resolved.flatMap { (option, text) ->
            option.choices.zip(text.choices).filter { (source, localized) ->
                localized.label == source.label && !isTechnical(source.label)
            }.map { (source, _) -> "${option.key}:${source.label}" }
        }
        return Coverage(
            language = language,
            optionCount = options.size,
            labelCount = labels.size,
            labelTotal = options.size,
            choiceCount = choices.size,
            choiceTotal = options.sumOf { it.choices.size },
            untranslatedLabels = untranslatedLabels,
            untranslatedChoices = untranslatedChoices,
        )
    }

    fun coverageAll(assets: AssetManager): List<Coverage> =
        resourceLanguages.map { coverage(assets, it) }

    /** Lookup helpers for callers that only have a persisted option key/value. */
    fun optionText(context: Context, key: String): Text? =
        PpssppCoreOptions.option(key)?.let { resolve(context, it) }

    fun localizedLabel(context: Context, key: String): String? =
        optionText(context, key)?.label

    fun localizedChoiceLabel(context: Context, key: String, value: String): String? =
        optionText(context, key)?.choices?.firstOrNull { it.value == value }?.label

    private fun loadCatalog(assets: AssetManager, language: String): Catalog {
        val file = languageFiles[language] ?: languageFiles.getValue("en")
        val values = LinkedHashMap<String, String>()
        if (file.isBlank()) return Catalog(emptyMap())
        runCatching {
            assets.open("lang/$file").bufferedReader().useLines { lines ->
                lines.forEach { raw ->
                    val line = raw.trim()
                    if (line.isEmpty() || line.startsWith('#') || line.startsWith('[')) return@forEach
                    val split = line.indexOf('=')
                    if (split <= 0) return@forEach
                    val key = line.substring(0, split).trim()
                    val value = line.substring(split + 1).trim()
                    // PPSSPP's desktop translations mark keyboard accelerators
                    // with '&'. Android has no such shortcut in these controls.
                    values.putIfAbsent(normalize(key), value.replace("&", ""))
                }
            }
        }
        return Catalog(values)
    }

    private val resourceLanguages = listOf(
        "ar", "cs", "de", "es", "fa", "fr", "hi", "in", "it", "ja", "ko", "pl", "pt", "ru", "tr", "uk", "zh",
    )

    private fun isTechnical(value: String): Boolean =
        value.matches(Regex("^(?:[A-Z0-9][A-Za-z0-9+._:/()%-]*|[0-9]+(?:\\.[0-9]+)?%?|[xX][0-9]+)$")) ||
            value.matches(Regex("^[0-9]+x \\([0-9]+x[0-9]+\\)$")) ||
            value.matches(Regex("^[a-f]$")) ||
            value.matches(Regex("^(?:[a-z0-9-]+\\.)+[a-z]{2,}$")) ||
            value == "localhost" || value == "xBRZ" ||
            value.contains("PSP", ignoreCase = false) || value.contains("UPnP", ignoreCase = false)

    private fun Catalog.lookup(key: String): String? = values[normalize(key)]

    private fun choiceLabel(
        language: String,
        catalog: Catalog,
        choice: PpssppCoreOptions.Choice,
    ): String {
        val value = choice.value
        val direct = catalog.lookup(choice.label)
        if (direct != null && (direct != choice.label || isTechnical(choice.label))) return direct
        val alias = when {
            value.equals("enabled", ignoreCase = true) -> "On"
            value.equals("disabled", ignoreCase = true) -> "Off"
            value.equals("Automatic", ignoreCase = true) -> "Auto"
            choice.label == "Dynarec (JIT)" -> "Dynarec/JIT (recommended)"
            choice.label == "IR Interpreter" -> "JIT using IR"
            choice.label == "Cross" -> "Use X to confirm"
            choice.label == "Circle" -> "Use O to confirm"
            choice.label == "Fast" -> "Fast"
            choice.label == "Host" -> "Host (bugs, less lag)"
            choice.label == "Simulate UMD delays" -> "Simulate UMD delays"
            choice.label == "Number of frames" -> "Number of Frames"
            choice.label == "Percent of FPS" -> "Percent of FPS"
            choice.label == "No buffer" -> "No buffer"
            choice.label == "Up to 1" -> "Up to 1"
            choice.label == "Low" -> "Low"
            choice.label == "Medium" -> "Medium"
            choice.label == "High" -> "High"
            choice.label == "Safe" -> "Safe"
            choice.label == "Balanced" -> "Balanced"
            choice.label == "Aggressive" -> "Aggressive"
            choice.label == "xBRZ" -> "xBRZ"
            choice.label == "Hybrid + Bicubic" -> "Hybrid + Bicubic"
            else -> null
        }
        return alias?.let { catalog.lookup(it) }
            ?.takeUnless { it == choice.label && !isTechnical(choice.label) }
            ?: fallbackChoice(language, choice.label)
    }

    private fun fallbackLabel(language: String, catalog: Catalog, option: PpssppCoreOptions.Option): String {
        val number = Regex("Pt\\s+([0-9]+)").find(option.label)?.groupValues?.get(1)
        if (option.key.startsWith("ppsspp_change_mac_address")) {
            val prefix = catalog.lookup("MAC address") ?: translateFallback(language, "MAC address")
            val part = if (language == "hi") "भाग" else "Pt"
            return "$prefix $part ${number ?: ""}:" + option.label.substringAfter(':')
        }
        if (option.key.startsWith("ppsspp_pro_ad_hoc_server_address")) {
            val prefix = catalog.lookup("Ad hoc server address") ?: translateFallback(language, "Ad hoc server address")
            val part = if (language == "hi") "भाग" else "Pt"
            return "$prefix $part ${number ?: ""}:" + option.label.substringAfter(':')
        }
        val custom = customLabels[language]?.get(option.key)
            ?: specialLabels[language]?.get(option.key)
        return custom ?: translateFallback(language, option.label)
    }

    private val memoryStickSizeLabels = mapOf(
        "ar" to "سعة ذاكرة PSP", "cs" to "Kapacita paměťové karty", "de" to "Memory-Stick-Kapazität",
        "es" to "Capacidad de Memory Stick", "fa" to "ظرفیت مموری استیک", "fr" to "Capacité du Memory Stick",
        "hi" to "मेमोरी स्टिक क्षमता", "in" to "Kapasitas Memory Stick", "it" to "Capacità Memory Stick",
        "ja" to "メモリースティックの容量", "ko" to "메모리 스틱 용량", "pl" to "Pojemność Memory Stick",
        "pt" to "Capacidade do Memory Stick", "ru" to "Ёмкость карты памяти", "tr" to "Memory Stick kapasitesi",
        "uk" to "Обсяг карти пам’яті", "zh" to "記憶棒容量",
    )

    private val specialLabels = mapOf(
        "cs" to mapOf("ppsspp_skip_gpu_readbacks" to "Přeskočit zpětné čtení GPU"),
        "fa" to mapOf("ppsspp_skip_gpu_readbacks" to "رد کردن بازخوانی GPU"),
        "in" to mapOf("ppsspp_skip_gpu_readbacks" to "Lewati pembacaan balik GPU"),
        "pt" to mapOf("ppsspp_enable_builtin_pro_ad_hoc_server" to "Ativar servidor PRO Ad Hoc integrado"),
        "hi" to mapOf(
            "ppsspp_memstick_size" to "मेमोरी स्टिक क्षमता",
            "ppsspp_texture_scaling_type" to "टेक्सचर स्केलिंग प्रकार",
            "ppsspp_texture_scaling_level" to "टेक्सचर स्केलिंग स्तर",
            "ppsspp_internal_resolution" to "रेंडरिंग रिज़ॉल्यूशन",
            "ppsspp_mulitsample_level" to "MSAA एंटीएलियासिंग",
            "ppsspp_button_preference" to "पुष्टि बटन",
            "ppsspp_analog_sensitivity" to "एनालॉग अक्ष स्केल",
            "ppsspp_spline_quality" to "स्प्लाइन/बेज़ियर वक्र गुणवत्ता",
            "ppsspp_gpu_hardware_transform" to "हार्डवेयर ट्रांसफ़ॉर्म",
            "ppsspp_hardware_tesselation" to "हार्डवेयर टेसेलेशन",
            "ppsspp_enable_builtin_pro_ad_hoc_server" to "अंतर्निहित PRO Ad Hoc सर्वर सक्षम करें",
        ),
    )

    private fun fallbackChoice(language: String, label: String): String =
        choiceTranslations[language]?.get(label)
            ?: specialChoiceTranslations[language]?.get(label)
            ?: translateFallback(language, label)

    private val specialChoiceTranslations = mapOf(
        "ar" to mapOf("Percent of FPS" to "نسبة معدل الإطارات", "IP address" to "عنوان IP"),
        "cs" to mapOf("Percent of FPS" to "Procento snímkové frekvence", "IP address" to "IP adresa", "Number of frames" to "Počet snímků", "No buffer" to "Bez vyrovnávací paměti", "Up to 1" to "Až 1"),
        "de" to mapOf("Percent of FPS" to "Prozent der Bildrate", "IP address" to "IP-Adresse"),
        "es" to mapOf("Percent of FPS" to "Porcentaje de FPS", "IP address" to "Dirección IP"),
        "fa" to mapOf("Percent of FPS" to "درصد نرخ فریم", "IP address" to "نشانی IP", "Number of frames" to "تعداد فریم‌ها", "Up to 1" to "تا ۱", "Hybrid + Bicubic" to "ترکیبی + دو‌مکعبی"),
        "fr" to mapOf("Percent of FPS" to "Pourcentage de FPS", "IP address" to "Adresse IP"),
        "hi" to mapOf("Percent of FPS" to "FPS का प्रतिशत", "IP address" to "IP पता", "Dynarec (JIT)" to "डायनारेक (JIT)", "IR Interpreter" to "IR इंटरप्रेटर", "Chinese Traditional" to "पारंपरिक चीनी", "Chinese Simplified" to "सरलीकृत चीनी", "Hybrid + Bicubic" to "हाइब्रिड + बाइक्यूबिक", "Auto max quality" to "स्वतः अधिकतम गुणवत्ता"),
        "in" to mapOf("Percent of FPS" to "Persentase FPS", "IP address" to "Alamat IP"),
        "it" to mapOf("Percent of FPS" to "Percentuale degli FPS", "IP address" to "Indirizzo IP"),
        "ja" to mapOf("Percent of FPS" to "FPS の割合", "IP address" to "IP アドレス"),
        "ko" to mapOf("Percent of FPS" to "FPS 백분율", "IP address" to "IP 주소"),
        "pl" to mapOf("Percent of FPS" to "Procent liczby klatek", "IP address" to "Adres IP"),
        "pt" to mapOf("Percent of FPS" to "Percentual de FPS", "IP address" to "Endereço IP"),
        "ru" to mapOf("Percent of FPS" to "Процент частоты кадров", "IP address" to "IP-адрес"),
        "tr" to mapOf("Percent of FPS" to "FPS yüzdesi", "IP address" to "IP adresi"),
        "uk" to mapOf("Percent of FPS" to "Відсоток частоти кадрів", "IP address" to "IP-адреса"),
        "zh" to mapOf("Percent of FPS" to "幀率百分比", "IP address" to "IP 位址"),
    )

    private fun normalize(value: String): String =
        value.lowercase(Locale.ROOT).replace(Regex("[^a-z0-9]+"), "")

    // These are the few libretro-only labels which do not have a PPSSPP key.
    // Address masks and numeric values remain unchanged by design.
    private val customLabels: Map<String, Map<String, String>> = mapOf(
        "ar" to mapOf(
            "ppsspp_cropto16x9" to "اقتصاص إلى 16:9", "ppsspp_frameskiptype" to "نوع تخطي الإطارات",
            "ppsspp_frame_duplication" to "عرض الإطارات المكررة عند 60 هرتز", "ppsspp_detect_vsync_swap_interval" to "اكتشاف تغييرات معدل الإطارات",
            "ppsspp_inflight_frames" to "تخزين أوامر الرسومات مؤقتًا", "ppsspp_analog_is_circular" to "تعويض بوابة العصا التناظرية",
            "ppsspp_lazy_texture_caching" to "التخزين المؤقت الكسول للخامات", "ppsspp_lower_resolution_for_effects" to "خفض دقة المؤثرات",
            "ppsspp_software_skinning" to "معالجة الجلد برمجيًا", "ppsspp_texture_shader" to "تظليل الخامات",
            "ppsspp_change_pro_ad_hoc_server_address" to "تغيير عنوان خادم PRO Ad Hoc", "ppsspp_enable_upnp" to "تفعيل UPnP",
        ),
        "fa" to mapOf(
            "ppsspp_cropto16x9" to "برش به 16:9", "ppsspp_frameskiptype" to "نوع رد کردن فریم", "ppsspp_frame_duplication" to "رندر فریم‌های تکراری با 60 هرتز", "ppsspp_detect_vsync_swap_interval" to "تشخیص تغییرات نرخ فریم", "ppsspp_inflight_frames" to "بافر کردن فرمان‌های گرافیکی", "ppsspp_analog_is_circular" to "جبران دروازه دایره‌ای آنالوگ", "ppsspp_lazy_texture_caching" to "کش تنبل بافت‌ها", "ppsspp_lower_resolution_for_effects" to "کاهش وضوح جلوه‌ها", "ppsspp_software_skinning" to "اسکینینگ نرم‌افزاری", "ppsspp_texture_shader" to "شیدر بافت", "ppsspp_change_pro_ad_hoc_server_address" to "تغییر نشانی سرور PRO Ad Hoc", "ppsspp_enable_upnp" to "فعال‌سازی UPnP"
        ),
        "cs" to mapOf("ppsspp_cropto16x9" to "Oříznout na 16:9", "ppsspp_frameskiptype" to "Typ přeskakování snímků", "ppsspp_frame_duplication" to "Vykreslit duplicitní snímky na 60 Hz", "ppsspp_detect_vsync_swap_interval" to "Detekovat změny snímkové frekvence", "ppsspp_inflight_frames" to "Vyrovnávací paměť příkazů grafiky", "ppsspp_analog_is_circular" to "Kompenzace kruhové brány analogu", "ppsspp_lazy_texture_caching" to "Líné ukládání textur", "ppsspp_lower_resolution_for_effects" to "Snížit rozlišení efektů", "ppsspp_software_skinning" to "Softwarové skinování", "ppsspp_texture_shader" to "Shader textur", "ppsspp_change_pro_ad_hoc_server_address" to "Změnit adresu PRO Ad Hoc serveru", "ppsspp_enable_upnp" to "Povolit UPnP"),
        "de" to mapOf("ppsspp_cropto16x9" to "Auf 16:9 zuschneiden", "ppsspp_frameskiptype" to "Frameskip-Typ", "ppsspp_frame_duplication" to "Doppelte Frames mit 60 Hz rendern", "ppsspp_detect_vsync_swap_interval" to "Änderungen der Bildrate erkennen", "ppsspp_inflight_frames" to "Grafikbefehle puffern", "ppsspp_analog_is_circular" to "Ausgleich für kreisförmige Analogbegrenzung", "ppsspp_lazy_texture_caching" to "Träges Textur-Caching", "ppsspp_lower_resolution_for_effects" to "Effektauflösung reduzieren", "ppsspp_software_skinning" to "Software-Skinning", "ppsspp_texture_shader" to "Textur-Shader", "ppsspp_change_pro_ad_hoc_server_address" to "PRO-Ad-Hoc-Serveradresse ändern", "ppsspp_enable_upnp" to "UPnP aktivieren"),
        "es" to mapOf("ppsspp_cropto16x9" to "Recortar a 16:9", "ppsspp_frameskiptype" to "Tipo de salto de fotogramas", "ppsspp_frame_duplication" to "Renderizar fotogramas duplicados a 60 Hz", "ppsspp_detect_vsync_swap_interval" to "Detectar cambios de frecuencia", "ppsspp_inflight_frames" to "Almacenar comandos gráficos", "ppsspp_analog_is_circular" to "Compensación de la guía circular analógica", "ppsspp_lazy_texture_caching" to "Caché de texturas diferida", "ppsspp_lower_resolution_for_effects" to "Bajar resolución de efectos", "ppsspp_software_skinning" to "Skinning por software", "ppsspp_texture_shader" to "Sombreado de texturas", "ppsspp_change_pro_ad_hoc_server_address" to "Cambiar dirección del servidor PRO Ad Hoc", "ppsspp_enable_upnp" to "Activar UPnP"),
        "fr" to mapOf("ppsspp_cropto16x9" to "Rogner en 16:9", "ppsspp_frameskiptype" to "Type de saut d’images", "ppsspp_frame_duplication" to "Rendre les images dupliquées à 60 Hz", "ppsspp_detect_vsync_swap_interval" to "Détecter les changements de fréquence", "ppsspp_inflight_frames" to "Mettre en tampon les commandes graphiques", "ppsspp_analog_is_circular" to "Compensation de la zone circulaire analogique", "ppsspp_lazy_texture_caching" to "Mise en cache différée des textures", "ppsspp_lower_resolution_for_effects" to "Réduire la résolution des effets", "ppsspp_software_skinning" to "Skinning logiciel", "ppsspp_texture_shader" to "Shader de texture", "ppsspp_change_pro_ad_hoc_server_address" to "Modifier l’adresse du serveur PRO Ad Hoc", "ppsspp_enable_upnp" to "Activer UPnP"),
        "it" to mapOf("ppsspp_cropto16x9" to "Ritaglia a 16:9", "ppsspp_frameskiptype" to "Tipo di salto fotogrammi", "ppsspp_frame_duplication" to "Renderizza fotogrammi duplicati a 60 Hz", "ppsspp_detect_vsync_swap_interval" to "Rileva cambi di frequenza", "ppsspp_inflight_frames" to "Buffer dei comandi grafici", "ppsspp_analog_is_circular" to "Compensazione guida circolare analogica", "ppsspp_lazy_texture_caching" to "Cache texture differita", "ppsspp_lower_resolution_for_effects" to "Riduci risoluzione effetti", "ppsspp_software_skinning" to "Skinning software", "ppsspp_texture_shader" to "Shader texture", "ppsspp_change_pro_ad_hoc_server_address" to "Cambia indirizzo server PRO Ad Hoc", "ppsspp_enable_upnp" to "Abilita UPnP"),
        "pt" to mapOf("ppsspp_cropto16x9" to "Cortar para 16:9", "ppsspp_frameskiptype" to "Tipo de salto de quadros", "ppsspp_frame_duplication" to "Renderizar quadros duplicados a 60 Hz", "ppsspp_detect_vsync_swap_interval" to "Detectar mudanças na taxa de quadros", "ppsspp_inflight_frames" to "Armazenar comandos gráficos", "ppsspp_analog_is_circular" to "Compensação da guia circular analógica", "ppsspp_lazy_texture_caching" to "Cache de texturas preguiçoso", "ppsspp_lower_resolution_for_effects" to "Reduzir resolução dos efeitos", "ppsspp_software_skinning" to "Skinning por software", "ppsspp_texture_shader" to "Shader de textura", "ppsspp_change_pro_ad_hoc_server_address" to "Alterar endereço do servidor PRO Ad Hoc", "ppsspp_enable_upnp" to "Ativar UPnP"),
        "ru" to mapOf("ppsspp_cropto16x9" to "Обрезка до 16:9", "ppsspp_frameskiptype" to "Тип пропуска кадров", "ppsspp_frame_duplication" to "Вывод дублированных кадров в 60 Гц", "ppsspp_detect_vsync_swap_interval" to "Определять изменения частоты кадров", "ppsspp_inflight_frames" to "Буферизация графических команд", "ppsspp_analog_is_circular" to "Компенсация круглой зоны аналогового стика", "ppsspp_lazy_texture_caching" to "Отложенное кэширование текстур", "ppsspp_lower_resolution_for_effects" to "Снизить разрешение эффектов", "ppsspp_software_skinning" to "Программная обработка скелета", "ppsspp_texture_shader" to "Шейдер текстур", "ppsspp_change_pro_ad_hoc_server_address" to "Изменить адрес PRO Ad Hoc сервера", "ppsspp_enable_upnp" to "Включить UPnP"),
        "uk" to mapOf("ppsspp_cropto16x9" to "Обрізати до 16:9", "ppsspp_frameskiptype" to "Тип пропуску кадрів", "ppsspp_frame_duplication" to "Виводити дубльовані кадри у 60 Гц", "ppsspp_detect_vsync_swap_interval" to "Виявляти зміни частоти кадрів", "ppsspp_inflight_frames" to "Буферизація графічних команд", "ppsspp_analog_is_circular" to "Компенсація круглої зони аналога", "ppsspp_lazy_texture_caching" to "Відкладене кешування текстур", "ppsspp_lower_resolution_for_effects" to "Знизити роздільність ефектів", "ppsspp_software_skinning" to "Програмна обробка скелета", "ppsspp_texture_shader" to "Шейдер текстур", "ppsspp_change_pro_ad_hoc_server_address" to "Змінити адресу PRO Ad Hoc сервера", "ppsspp_enable_upnp" to "Увімкнути UPnP"),
        "pl" to mapOf("ppsspp_cropto16x9" to "Przytnij do 16:9", "ppsspp_frameskiptype" to "Typ pomijania klatek", "ppsspp_frame_duplication" to "Renderuj duplikaty klatek w 60 Hz", "ppsspp_detect_vsync_swap_interval" to "Wykrywaj zmiany liczby klatek", "ppsspp_inflight_frames" to "Buforuj polecenia grafiki", "ppsspp_analog_is_circular" to "Kompensacja kołowej bramki analoga", "ppsspp_lazy_texture_caching" to "Leniwe buforowanie tekstur", "ppsspp_lower_resolution_for_effects" to "Obniż rozdzielczość efektów", "ppsspp_software_skinning" to "Skinning programowy", "ppsspp_texture_shader" to "Shader tekstur", "ppsspp_change_pro_ad_hoc_server_address" to "Zmień adres serwera PRO Ad Hoc", "ppsspp_enable_upnp" to "Włącz UPnP"),
        "tr" to mapOf("ppsspp_cropto16x9" to "16:9'a kırp", "ppsspp_frameskiptype" to "Kare atlama türü", "ppsspp_frame_duplication" to "Yinelenen kareleri 60 Hz'de oluştur", "ppsspp_detect_vsync_swap_interval" to "Kare hızı değişikliklerini algıla", "ppsspp_inflight_frames" to "Grafik komutlarını arabelleğe al", "ppsspp_analog_is_circular" to "Analog dairesel kapı telafisi", "ppsspp_lazy_texture_caching" to "Tembel doku önbelleği", "ppsspp_lower_resolution_for_effects" to "Efekt çözünürlüğünü düşür", "ppsspp_software_skinning" to "Yazılım skinning", "ppsspp_texture_shader" to "Doku gölgelendiricisi", "ppsspp_change_pro_ad_hoc_server_address" to "PRO Ad Hoc sunucu adresini değiştir", "ppsspp_enable_upnp" to "UPnP'yi etkinleştir"),
        "ko" to mapOf("ppsspp_cropto16x9" to "16:9으로 자르기", "ppsspp_frameskiptype" to "프레임 건너뛰기 유형", "ppsspp_frame_duplication" to "60Hz로 중복 프레임 렌더링", "ppsspp_detect_vsync_swap_interval" to "프레임 속도 변경 감지", "ppsspp_inflight_frames" to "그래픽 명령 버퍼링", "ppsspp_analog_is_circular" to "아날로그 원형 게이트 보정", "ppsspp_lazy_texture_caching" to "지연 텍스처 캐싱", "ppsspp_lower_resolution_for_effects" to "효과 해상도 낮추기", "ppsspp_software_skinning" to "소프트웨어 스키닝", "ppsspp_texture_shader" to "텍스처 셰이더", "ppsspp_change_pro_ad_hoc_server_address" to "PRO Ad Hoc 서버 주소 변경", "ppsspp_enable_upnp" to "UPnP 사용"),
        "ja" to mapOf("ppsspp_cropto16x9" to "16:9 に切り抜く", "ppsspp_frameskiptype" to "フレームスキップの種類", "ppsspp_frame_duplication" to "60Hz 用に重複フレームを描画", "ppsspp_detect_vsync_swap_interval" to "フレームレートの変化を検出", "ppsspp_inflight_frames" to "グラフィックスコマンドをバッファ", "ppsspp_analog_is_circular" to "アナログ円形ゲート補正", "ppsspp_lazy_texture_caching" to "テクスチャの遅延キャッシュ", "ppsspp_lower_resolution_for_effects" to "エフェクトの解像度を下げる", "ppsspp_software_skinning" to "ソフトウェアスキニング", "ppsspp_texture_shader" to "テクスチャシェーダー", "ppsspp_change_pro_ad_hoc_server_address" to "PRO Ad Hoc サーバーアドレスを変更", "ppsspp_enable_upnp" to "UPnP を有効化"),
        "zh" to mapOf("ppsspp_cropto16x9" to "裁剪为 16:9", "ppsspp_frameskiptype" to "跳帧类型", "ppsspp_frame_duplication" to "以 60 Hz 渲染重复帧", "ppsspp_detect_vsync_swap_interval" to "检测帧率变化", "ppsspp_inflight_frames" to "缓冲图形命令", "ppsspp_analog_is_circular" to "模拟摇杆圆形边界补偿", "ppsspp_lazy_texture_caching" to "延迟纹理缓存", "ppsspp_lower_resolution_for_effects" to "降低特效分辨率", "ppsspp_software_skinning" to "软件蒙皮", "ppsspp_texture_shader" to "纹理着色器", "ppsspp_change_pro_ad_hoc_server_address" to "更改 PRO Ad Hoc 服务器地址", "ppsspp_enable_upnp" to "启用 UPnP"),
        "hi" to mapOf("ppsspp_cropto16x9" to "16:9 में क्रॉप करें", "ppsspp_frameskiptype" to "फ़्रेम स्किप प्रकार", "ppsspp_frame_duplication" to "60 Hz पर डुप्लिकेट फ़्रेम रेंडर करें", "ppsspp_detect_vsync_swap_interval" to "फ़्रेम दर परिवर्तन पहचानें", "ppsspp_inflight_frames" to "ग्राफ़िक्स कमांड बफ़र करें", "ppsspp_analog_is_circular" to "एनालॉग गोल गेट क्षतिपूर्ति", "ppsspp_lazy_texture_caching" to "लेज़ी टेक्सचर कैशिंग", "ppsspp_lower_resolution_for_effects" to "इफ़ेक्ट रिज़ॉल्यूशन घटाएँ", "ppsspp_software_skinning" to "सॉफ़्टवेयर स्किनिंग", "ppsspp_texture_shader" to "टेक्सचर शेडर", "ppsspp_change_pro_ad_hoc_server_address" to "PRO Ad Hoc सर्वर पता बदलें", "ppsspp_enable_upnp" to "UPnP सक्षम करें"),
        "in" to mapOf("ppsspp_cropto16x9" to "Pangkas ke 16:9", "ppsspp_frameskiptype" to "Jenis lewati bingkai", "ppsspp_frame_duplication" to "Render bingkai duplikat pada 60 Hz", "ppsspp_detect_vsync_swap_interval" to "Deteksi perubahan laju bingkai", "ppsspp_inflight_frames" to "Buffer perintah grafis", "ppsspp_analog_is_circular" to "Kompensasi gerbang analog melingkar", "ppsspp_lazy_texture_caching" to "Cache tekstur malas", "ppsspp_lower_resolution_for_effects" to "Turunkan resolusi efek", "ppsspp_software_skinning" to "Skinning perangkat lunak", "ppsspp_texture_shader" to "Shader tekstur", "ppsspp_change_pro_ad_hoc_server_address" to "Ubah alamat server PRO Ad Hoc", "ppsspp_enable_upnp" to "Aktifkan UPnP"),
    )

    private val choiceTranslations: Map<String, Map<String, String>> = mapOf(
        "uk" to mapOf("disabled" to "вимкнено", "enabled" to "увімкнено", "Automatic" to "Автоматично", "Cross" to "Хрестик", "Circle" to "Коло"),
        "ru" to mapOf("disabled" to "выключено", "enabled" to "включено", "Automatic" to "Автоматически", "Cross" to "Крестик", "Circle" to "Круг"),
        "pl" to mapOf("disabled" to "wyłączone", "enabled" to "włączone", "Automatic" to "Automatycznie", "Cross" to "Krzyżyk", "Circle" to "Kółko"),
        "de" to mapOf("disabled" to "deaktiviert", "enabled" to "aktiviert", "Automatic" to "Automatisch", "Cross" to "Kreuz", "Circle" to "Kreis"),
        "fr" to mapOf("disabled" to "désactivé", "enabled" to "activé", "Automatic" to "Automatique", "Cross" to "Croix", "Circle" to "Cercle"),
        "es" to mapOf("disabled" to "desactivado", "enabled" to "activado", "Automatic" to "Automático", "Cross" to "Cruz", "Circle" to "Círculo"),
        "it" to mapOf("disabled" to "disabilitato", "enabled" to "abilitato", "Automatic" to "Automatico", "Cross" to "Croce", "Circle" to "Cerchio"),
        "pt" to mapOf("disabled" to "desativado", "enabled" to "ativado", "Automatic" to "Automático", "Cross" to "Cruz", "Circle" to "Círculo"),
        "tr" to mapOf("disabled" to "devre dışı", "enabled" to "etkin", "Automatic" to "Otomatik", "Cross" to "Çarpı", "Circle" to "Daire"),
        "cs" to mapOf("disabled" to "zakázáno", "enabled" to "povoleno", "Automatic" to "Automaticky", "Cross" to "Křížek", "Circle" to "Kruh"),
        "ar" to mapOf("disabled" to "معطل", "enabled" to "مفعل", "Automatic" to "تلقائي", "Cross" to "تقاطع", "Circle" to "دائرة"),
        "fa" to mapOf("disabled" to "غیرفعال", "enabled" to "فعال", "Automatic" to "خودکار", "Cross" to "ضربدر", "Circle" to "دایره"),
        "ja" to mapOf("disabled" to "無効", "enabled" to "有効", "Automatic" to "自動", "Cross" to "×", "Circle" to "○"),
        "ko" to mapOf("disabled" to "비활성화", "enabled" to "활성화", "Automatic" to "자동", "Cross" to "십자", "Circle" to "원"),
        "zh" to mapOf("disabled" to "禁用", "enabled" to "启用", "Automatic" to "自动", "Cross" to "叉", "Circle" to "圆"),
        "hi" to mapOf(
            "disabled" to "अक्षम", "enabled" to "सक्षम", "Automatic" to "स्वचालित", "Cross" to "क्रॉस", "Circle" to "वृत्त",
            "Fast" to "तेज़", "Host" to "होस्ट", "Host (bugs, less lag)" to "होस्ट (कुछ त्रुटियाँ, कम विलंब)",
            "Simulate UMD delays" to "UMD विलंब का अनुकरण करें", "Number of frames" to "फ़्रेमों की संख्या",
            "Percent of FPS" to "FPS का प्रतिशत", "No buffer" to "बफ़र नहीं", "Up to 1" to "1 तक",
            "Low" to "कम", "Medium" to "मध्यम", "High" to "उच्च", "Safe" to "सुरक्षित",
            "Balanced" to "संतुलित", "Aggressive" to "आक्रामक", "Use X to confirm" to "पुष्टि के लिए X दबाएँ",
            "Use O to confirm" to "पुष्टि के लिए O दबाएँ", "JIT using IR" to "IR का उपयोग करने वाला JIT",
            "Dynarec/JIT (recommended)" to "Dynarec/JIT (अनुशंसित)", "Interpreter" to "इंटरप्रेटर",
        ),
        "in" to mapOf("disabled" to "dinonaktifkan", "enabled" to "diaktifkan", "Automatic" to "Otomatis", "Cross" to "Silang", "Circle" to "Lingkaran"),
    )

    private val wordTranslations: Map<String, Map<String, String>> = mapOf(
        "uk" to mapOf("Unstable" to "Нестабільно", "Slower" to "Повільніше", "less lag" to "менше затримки", "Core restart required" to "Потрібен перезапуск ядра", "Automatic" to "Автоматично", "frontend" to "фронтенду", "Slow, accurate" to "Повільно, точно", "Additional" to "Додаткова", "deadzone" to "мертва зона", "sensitivity" to "чутливість", "Faster" to "Швидше", "games" to "іграх", "effects" to "ефектів", "resolution" to "роздільність", "textures" to "текстур", "Only used" to "Використовується лише", "smoothness" to "плавність", "curves" to "кривих"),
        "ru" to mapOf("Unstable" to "Нестабильно", "Slower" to "Медленнее", "less lag" to "меньше задержки", "Core restart required" to "Требуется перезапуск ядра", "Automatic" to "Автоматически", "frontend" to "фронтенда", "Slow, accurate" to "Медленно, точно", "Additional" to "Дополнительная", "deadzone" to "мёртвая зона", "sensitivity" to "чувствительность", "Faster" to "Быстрее", "games" to "играх", "effects" to "эффектов", "resolution" to "разрешение", "textures" to "текстур", "Only used" to "Используется только", "smoothness" to "плавность", "curves" to "кривых"),
        "de" to mapOf("Unstable" to "Instabil", "Slower" to "Langsamer", "less lag" to "weniger Verzögerung", "Core restart required" to "Neustart des Kerns erforderlich", "Automatic" to "Automatisch", "frontend" to "Frontends", "Slow, accurate" to "Langsam, genau", "Additional" to "Zusätzliche", "deadzone" to "Totzone", "sensitivity" to "Empfindlichkeit", "Faster" to "Schneller", "games" to "Spielen", "effects" to "Effekte", "resolution" to "Auflösung", "textures" to "Texturen", "Only used" to "Nur verwendet", "smoothness" to "Glätte", "curves" to "Kurven"),
        "fr" to mapOf("Unstable" to "Instable", "Slower" to "Plus lent", "less lag" to "moins de latence", "Core restart required" to "Redémarrage du cœur requis", "Automatic" to "Automatique", "frontend" to "frontend", "Slow, accurate" to "Lent et précis", "Additional" to "Supplémentaire", "deadzone" to "zone morte", "sensitivity" to "sensibilité", "Faster" to "Plus rapide", "games" to "jeux", "effects" to "effets", "resolution" to "résolution", "textures" to "textures", "Only used" to "Utilisé uniquement", "smoothness" to "lissage", "curves" to "courbes"),
        "es" to mapOf("Unstable" to "Inestable", "Slower" to "Más lento", "less lag" to "menos latencia", "Core restart required" to "Requiere reiniciar el núcleo", "Automatic" to "Automático", "frontend" to "frontend", "Slow, accurate" to "Lento y preciso", "Additional" to "Adicional", "deadzone" to "zona muerta", "sensitivity" to "sensibilidad", "Faster" to "Más rápido", "games" to "juegos", "effects" to "efectos", "resolution" to "resolución", "textures" to "texturas", "Only used" to "Solo se usa", "smoothness" to "suavidad", "curves" to "curvas"),
        "hi" to mapOf(
            "CPU Core" to "CPU कोर", "Fast Memory" to "तेज़ मेमोरी", "Ignore Bad Memory Accesses" to "खराब मेमोरी पहुँच को अनदेखा करें",
            "I/O Timing Method" to "I/O टाइमिंग विधि", "Force Real Clock Sync" to "वास्तविक क्लॉक सिंक ज़बरदस्ती करें",
            "Locked CPU Speed" to "CPU गति लॉक करें", "Memory Stick Inserted" to "मेमोरी स्टिक डाली गई",
            "Cache Full ISO in RAM" to "पूरा ISO RAM में कैश करें",
            "Game Language" to "गेम भाषा", "PSP Language" to "PSP भाषा", "PSP Model" to "PSP मॉडल",
            "Backend" to "बैकएंड", "Graphics Backend" to "ग्राफ़िक्स बैकएंड", "Software Rendering" to "सॉफ़्टवेयर रेंडरिंग",
            "Internal Resolution" to "आंतरिक रिज़ॉल्यूशन", "Antialiasing (MSAA)" to "एंटीलियासिंग (MSAA)",
            "Crop to 16x9" to "16:9 में क्रॉप करें", "Crop to 16:9" to "16:9 में क्रॉप करें", "Frameskip" to "फ़्रेम स्किप",
            "Frameskip Type" to "फ़्रेम स्किप प्रकार", "Auto Frameskip" to "स्वचालित फ़्रेम स्किप", "Frame Duplication" to "फ़्रेम डुप्लिकेशन",
            "Detect VSync Swap Interval" to "VSync स्वैप अंतराल पहचानें", "Inflight Frames" to "इनफ़्लाइट फ़्रेम",
            "Analog Deadzone" to "एनालॉग डेडज़ोन", "Analog Sensitivity" to "एनालॉग संवेदनशीलता",
            "Skip Buffer Effects" to "बफ़र प्रभाव छोड़ें", "Skip GPU Readbacks" to "GPU रीडबैक छोड़ें",
            "Lazy Texture Caching" to "लेज़ी टेक्सचर कैशिंग", "Spline Quality" to "स्प्लाइन गुणवत्ता",
            "Lower Resolution for Effects" to "प्रभावों के लिए रिज़ॉल्यूशन घटाएँ", "Software Skinning" to "सॉफ़्टवेयर स्किनिंग",
            "Hardware Tessellation" to "हार्डवेयर टेसेलेशन", "Texture Scaling Level" to "टेक्सचर स्केलिंग स्तर",
            "Texture Deposterize" to "टेक्सचर डिपोस्टराइज़", "Texture Shader" to "टेक्सचर शेडर",
            "Smart 2D Texture Filtering" to "स्मार्ट 2D टेक्सचर फ़िल्टरिंग", "Texture Upscale Type" to "टेक्सचर अपस्केल प्रकार",
            "Texture Upscaling Level" to "टेक्सचर अपस्केल स्तर",
            "Texture Replacement" to "टेक्सचर प्रतिस्थापन", "Texture Filtering" to "टेक्सचर फ़िल्टरिंग", "Anisotropic Filtering" to "एनिसोट्रोपिक फ़िल्टरिंग",
            "Enable networking" to "नेटवर्किंग सक्षम करें", "Enable built-in ad hoc server" to "अंतर्निहित ad hoc सर्वर सक्षम करें",
            "Enable Networking/WLAN (Beta, may break games)" to "नेटवर्किंग/WLAN सक्षम करें (बीटा, गेम में समस्या हो सकती है)",
            "MAC address" to "MAC पता", "Ad hoc server address" to "Ad hoc सर्वर पता",
            "Change PRO Ad Hoc Server IP Address ('localhost' = multiple instances)" to "PRO Ad Hoc सर्वर IP पता बदलें ('localhost' = कई इंस्टेंस)",
            "Forced First Connect" to "पहला कनेक्शन ज़बरदस्ती करें", "UPnP Use Original Port ('ON' = PSP compatibility)" to "UPnP मूल पोर्ट का उपयोग करे ('ON' = PSP संगतता)",
            "Port Offset ('0' = PSP compatibility)" to "पोर्ट ऑफ़सेट ('0' = PSP संगतता)", "Minimum Timeout (Override in ms, '0' = default)" to "न्यूनतम टाइमआउट (ms में बदलें, '0' = डिफ़ॉल्ट)", "WLAN Channel" to "WLAN चैनल",
            "Unstable" to "अस्थिर", "Slower" to "धीमा", "less lag" to "कम विलंब", "Core restart required" to "कोर पुनः आरंभ आवश्यक",
            "Automatic" to "स्वचालित", "frontend" to "फ्रंटएंड", "Slow, accurate" to "धीमा, सटीक", "Additional" to "अतिरिक्त",
            "deadzone" to "डेडज़ोन", "sensitivity" to "संवेदनशीलता", "Faster" to "तेज़", "games" to "गेम", "effects" to "प्रभाव",
            "resolution" to "रिज़ॉल्यूशन", "textures" to "टेक्सचर", "Only used" to "केवल उपयोग", "smoothness" to "चिकनाई", "curves" to "वक्र",
        ),
    )

    private fun translateFallback(language: String, text: String): String {
        var result = text
        wordTranslations[language].orEmpty().entries
            .sortedByDescending { it.key.length }
            .forEach { (from, to) -> result = result.replace(from, to, ignoreCase = false) }
        return result
    }
}

package com.sbro.emucorea.ui.xmb

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.opengl.GLSurfaceView
import android.os.BatteryManager
import android.os.Build
import android.os.Environment
import android.os.StatFs
import android.view.MotionEvent
import android.view.WindowManager
import androidx.activity.compose.BackHandler
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.sbro.emucorea.R
import com.sbro.emucorea.core.PspGameMetadataReader
import com.sbro.emucorea.data.GameItem
import com.sbro.emucorea.data.SaveStateEntryInfo
import com.sbro.emucorea.data.SaveStateRepository
import com.sbro.emucorea.xmb.XmbAudio
import com.sbro.emucorea.xmb.XmbHost
import com.sbro.emucorea.xmb.XmbNative
import com.sbro.emucorea.xmb.XmbSurfaceRenderer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import kotlin.math.abs

data class GameAssets(
    val title: String,
    val iconPath: String,
    val pic1Path: String,
    val pic0Path: String,
    val wavPath: String,
    val serial: String,
    val version: String,
    val sysVersion: String,
    val region: String,
    val sizeStr: String,
    val dateStr: String,
)

private object XmbAssetCache {
    val games = ConcurrentHashMap<String, GameAssets>()

    @Volatile
    var lastSaves: List<SaveStateEntryInfo> = emptyList()

    fun keyFor(game: GameItem): String = "${game.path}|${game.fileSize}|${game.lastModified}"
}

private fun formatGameSize(sizeBytes: Long): String = when {
    sizeBytes <= 0L -> ""
    sizeBytes < 1024L * 1024L -> "${((sizeBytes + 1023L) / 1024L).coerceAtLeast(1L)} KB"
    else -> String.format(Locale.US, "%.1f MB", sizeBytes / (1024.0 * 1024.0))
}

private fun formatGameDate(modifiedMs: Long): String = if (modifiedMs > 0L) {
    SimpleDateFormat("dd/MM/yyyy HH:mm", Locale.getDefault()).format(Date(modifiedMs))
} else {
    ""
}

private fun hasValidCachedFiles(assets: GameAssets): Boolean =
    listOf(assets.iconPath, assets.pic1Path, assets.pic0Path, assets.wavPath)
        .filter { it.isNotEmpty() }
        .all { File(it).exists() }

private val XMB_STRING_BINDINGS: List<Pair<String, Int>> = listOf(
    "msgshare_settings" to R.string.xmb_cat_settings,
    "msgtop_extras" to R.string.xmb_cat_extras,
    "msgtop_photo" to R.string.xmb_cat_photo,
    "msgtop_music" to R.string.xmb_cat_music,
    "msgtop_video" to R.string.xmb_cat_video,
    "msgshare_game" to R.string.xmb_cat_game,
    "msgtop_network" to R.string.xmb_cat_network,
    "msg_psn" to R.string.xmb_cat_psn,
    "xmb_hint_enter" to R.string.xmb_hint_enter,
    "xmb_hint_back" to R.string.xmb_hint_back,
    "msg_yes" to R.string.xmb_yes,
    "msg_no" to R.string.xmb_no,
    "msg_exit_psp" to R.string.xmb_exit_confirm_title,
    "msg_feature_unavailable" to R.string.xmb_feature_unavailable,
    "msgtop_sysconf_update" to R.string.xmb_set_update,
    "msgtop_sysconf_usb" to R.string.xmb_set_usb,
    "msgtop_sysconf_video" to R.string.xmb_set_video,
    "msgtop_sysconf_photo" to R.string.xmb_set_photo,
    "msgtop_sysconf_console" to R.string.xmb_set_system,
    "msgtop_sysconf_theme" to R.string.xmb_set_theme,
    "msgtop_sysconf_date" to R.string.xmb_set_datetime,
    "msgtop_sysconf_powersave" to R.string.xmb_set_powersave,
    "msgtop_sysconf_sound" to R.string.xmb_set_sound,
    "msgtop_sysconf_security" to R.string.xmb_set_security,
    "msgtop_sysconf_network" to R.string.xmb_set_network,
    "msgtop_sysconf_rss" to R.string.xmb_set_rss,
    "msg_display_setting" to R.string.xmb_set_display,
    "msg_digitalcomics" to R.string.xmb_item_comics,
    "msg_1seg" to R.string.xmb_item_1seg,
    "msgtop_camera" to R.string.xmb_item_camera,
    "msgshare_ms" to R.string.xmb_item_memstick,
    "msgshare_info_space" to R.string.xmb_item_free_space,
    "msg_sensme_channels" to R.string.xmb_item_sensme,
    "msgtop_game_gamedl" to R.string.xmb_item_game_sharing,
    "msgtop_game_savedata" to R.string.xmb_item_savedata,
    "xmb_manuals" to R.string.xmb_manuals,
    "msgtop_network_lftv" to R.string.xmb_item_locationfree,
    "xmb_remote_play" to R.string.xmb_remote_play,
    "msgtop_network_rss" to R.string.xmb_item_rss,
    "msgtop_network_browser" to R.string.xmb_item_browser,
    "msg_internet_search" to R.string.xmb_item_search,
    "xmb_internet_radio" to R.string.xmb_internet_radio,
    "xmb_skype" to R.string.xmb_skype,
    "msg_signup" to R.string.xmb_item_psn_signup,
    "msg_ps_store" to R.string.xmb_item_ps_store,
    "msg_information_board" to R.string.xmb_item_info_board,
    "msg_delete_confirm_1" to R.string.xmb_delete_confirm_1,
    "msg_delete_confirm_2" to R.string.xmb_delete_confirm_2,
    "msg_no_saved_data" to R.string.xmb_no_saved_data,
    "msgshare_error_no_game" to R.string.xmb_no_games,
    "xmb_no_images" to R.string.xmb_no_images,
    "msg_no_images" to R.string.xmb_no_images,
    "xmb_no_tracks" to R.string.xmb_no_tracks,
    "xmb_no_videos" to R.string.xmb_no_videos,
    "msgshare_opt_load" to R.string.xmb_opt_start,
    "msgshare_opt_del" to R.string.xmb_opt_delete,
    "msgshare_opt_info" to R.string.xmb_opt_info,
    "msgshare_info_title" to R.string.xmb_info_title,
    "msgshare_info_size" to R.string.xmb_info_size,
    "msgshare_info_date" to R.string.xmb_info_date,
    "xmb_info_version" to R.string.xmb_info_version,
    "xmb_info_sysver" to R.string.xmb_info_sysver,
    "xmb_info_discid" to R.string.xmb_info_discid,
    "xmb_info_region" to R.string.xmb_info_region,
    "xmb_setting_theme" to R.string.xmb_setting_theme,
    "xmb_setting_color" to R.string.xmb_setting_color,
    "xmb_setting_background" to R.string.xmb_setting_background,
    "xmb_setting_wave" to R.string.xmb_setting_wave,
    "xmb_setting_sysinfo" to R.string.xmb_setting_sysinfo,
    "xmb_setting_confirm_btn" to R.string.xmb_setting_confirm_btn,
    "xmb_setting_keytone" to R.string.xmb_setting_keytone,
    "xmb_setting_bgm" to R.string.xmb_setting_bgm,
    "xmb_setting_date_fmt" to R.string.xmb_setting_date_fmt,
    "xmb_setting_time_fmt" to R.string.xmb_setting_time_fmt,
    "xmb_sysinfo_fw" to R.string.xmb_sysinfo_fw,
    "xmb_sysinfo_name" to R.string.xmb_sysinfo_name,
    "xmb_opt_original" to R.string.xmb_opt_original,
    "xmb_opt_classic" to R.string.xmb_opt_classic,
    "xmb_opt_month_auto" to R.string.xmb_opt_month_auto,
    "xmb_opt_slow" to R.string.xmb_opt_slow,
    "xmb_opt_static" to R.string.xmb_opt_static,
    "xmb_opt_on" to R.string.xmb_opt_on,
    "xmb_opt_off" to R.string.xmb_opt_off,
    "xmb_btn_cross_lbl" to R.string.xmb_btn_cross_lbl,
    "xmb_btn_circle_lbl" to R.string.xmb_btn_circle_lbl,
    "xmb_opt_24h" to R.string.xmb_opt_24h,
    "xmb_opt_12h" to R.string.xmb_opt_12h,
    "xmb_month_jan" to R.string.xmb_month_jan,
    "xmb_month_feb" to R.string.xmb_month_feb,
    "xmb_month_mar" to R.string.xmb_month_mar,
    "xmb_month_apr" to R.string.xmb_month_apr,
    "xmb_month_may" to R.string.xmb_month_may,
    "xmb_month_jun" to R.string.xmb_month_jun,
    "xmb_month_jul" to R.string.xmb_month_jul,
    "xmb_month_aug" to R.string.xmb_month_aug,
    "xmb_month_sep" to R.string.xmb_month_sep,
    "xmb_month_oct" to R.string.xmb_month_oct,
    "xmb_month_nov" to R.string.xmb_month_nov,
    "xmb_month_dec" to R.string.xmb_month_dec,
)

private fun pushLocalizedStrings(context: Context) {
    val keys = Array(XMB_STRING_BINDINGS.size) { index -> XMB_STRING_BINDINGS[index].first }
    val values = Array(XMB_STRING_BINDINGS.size) { index ->
        runCatching { context.getString(XMB_STRING_BINDINGS[index].second) }.getOrDefault("")
    }
    XmbNative.nativeSetStrings(keys, values)
}

private fun pushStorageInfo() {
    val freeText = runCatching {
        val stat = StatFs(Environment.getDataDirectory().path)
        val avail = stat.availableBytes
        if (avail >= 1024L * 1024L * 1024L) {
            String.format(Locale.US, "%.1f GB", avail / (1024.0 * 1024.0 * 1024.0))
        } else {
            "${(avail / (1024L * 1024L)).coerceAtLeast(0L)} MB"
        }
    }.getOrDefault("1459 MB")
    val deviceName = Build.MODEL.orEmpty().trim().ifBlank { "PSP\u2122" }
    XmbNative.nativeSetStorageInfo(freeText, deviceName)
}

private fun pushGamesToNative(assets: List<GameAssets>) {
    XmbNative.nativeSetGames(
        titles = assets.map { it.title }.toTypedArray(),
        icons = assets.map { it.iconPath }.toTypedArray(),
        pic1s = assets.map { it.pic1Path }.toTypedArray(),
        pic0s = assets.map { it.pic0Path }.toTypedArray(),
        wavs = assets.map { it.wavPath }.toTypedArray(),
        serials = assets.map { it.serial }.toTypedArray(),
        versions = assets.map { it.version }.toTypedArray(),
        sysVersions = assets.map { it.sysVersion }.toTypedArray(),
        regions = assets.map { it.region }.toTypedArray(),
        sizes = assets.map { it.sizeStr }.toTypedArray(),
        dates = assets.map { it.dateStr }.toTypedArray(),
    )
}

private fun pushSavesToNative(
    context: Context,
    saveRepo: SaveStateRepository,
    saves: List<SaveStateEntryInfo>,
) {
    val dateFormat = SimpleDateFormat("dd/MM/yyyy HH:mm", Locale.getDefault())
    val autoLabel = context.getString(R.string.xmb_slot_auto)
    val titles = saves.map { it.gameTitle.ifBlank { it.serial } }.toTypedArray()
    val slots = saves.map {
        if (it.isAutoSave) autoLabel else context.getString(R.string.xmb_slot_fmt, it.slot)
    }.toTypedArray()
    val dates = saves.map { dateFormat.format(Date(it.lastModified)) }.toTypedArray()
    val sizes = saves.map { "${((it.sizeBytes + 1023L) / 1024L).coerceAtLeast(1L)} KB" }.toTypedArray()
    val previews = saves.map { entry ->
        val repoPreview = runCatching { saveRepo.getPreviewImagePath(entry) }.getOrNull()
        val statePng = File("${entry.absolutePath}.png")
        when {
            !repoPreview.isNullOrBlank() && File(repoPreview).exists() -> repoPreview
            statePng.exists() -> statePng.absolutePath
            !entry.coverArtPath.isNullOrBlank() && File(entry.coverArtPath).exists() -> entry.coverArtPath
            else -> ""
        }
    }.toTypedArray()
    val savePaths = saves.map { it.absolutePath }.toTypedArray()
    val gamePaths = saves.map { it.gamePath.orEmpty() }.toTypedArray()
    XmbNative.nativeSetSaves(titles, slots, dates, sizes, previews, savePaths, gamePaths)
}

/**
 * Compose host for the authentic Sony PSP XMB mode.
 */
@Composable
fun XmbScreen(
    games: List<GameItem>,
    onGameSelected: (Int) -> Unit,
    onSaveSelected: ((gamePath: String, slot: Int) -> Unit)? = null,
    onDeleteGame: ((GameItem) -> Unit)? = null,
    onExitXmb: () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val coroutineScope = rememberCoroutineScope()
    val localeTag = context.resources.configuration.locales.toLanguageTags()

    val currentGames = rememberUpdatedState(games)
    val currentSelection = rememberUpdatedState(onGameSelected)
    val currentSaveSelection = rememberUpdatedState(onSaveSelected)
    val currentDeleteGame = rememberUpdatedState(onDeleteGame)
    val currentExit = rememberUpdatedState(onExitXmb)

    val initialGameAssets = remember(games) {
        if (games.isEmpty()) {
            emptyList()
        } else {
            games.map { game ->
                XmbAssetCache.games[XmbAssetCache.keyFor(game)] ?: GameAssets(
                    title = game.title,
                    iconPath = "",
                    pic1Path = "",
                    pic0Path = "",
                    wavPath = "",
                    serial = game.serial.orEmpty(),
                    version = "",
                    sysVersion = "",
                    region = "",
                    sizeStr = formatGameSize(game.fileSize),
                    dateStr = formatGameDate(game.lastModified),
                )
            }
        }
    }

    val gameAssets by produceState<List<GameAssets>>(initialValue = initialGameAssets, games, context) {
        value = if (games.isEmpty()) {
            emptyList()
        } else {
            withContext(Dispatchers.IO) {
                val semaphore = Semaphore(4)
                coroutineScope {
                    games.map { game ->
                        async {
                            val cacheKey = XmbAssetCache.keyFor(game)
                            val cached = XmbAssetCache.games[cacheKey]
                            if (cached != null && hasValidCachedFiles(cached)) {
                                return@async cached
                            }
                            semaphore.withPermit {
                                val icon = runCatching { PspGameMetadataReader.extractIcon0(context, game.path) }.getOrNull().orEmpty()
                                val pic1 = runCatching { PspGameMetadataReader.extractPic1(context, game.path) }.getOrNull().orEmpty()
                                val pic0 = runCatching { PspGameMetadataReader.extractPic0(context, game.path) }.getOrNull().orEmpty()
                                val wav = runCatching { PspGameMetadataReader.extractSnd0(context, game.path) }.getOrNull().orEmpty()
                                val ext = runCatching { PspGameMetadataReader.readExtendedInfo(context, game.path) }.getOrNull()
                                val sizeBytes = ext?.sizeBytes?.takeIf { it > 0L } ?: game.fileSize
                                val modifiedMs = ext?.lastModified?.takeIf { it > 0L } ?: game.lastModified
                                val resolved = GameAssets(
                                    title = game.title,
                                    iconPath = icon,
                                    pic1Path = pic1,
                                    pic0Path = pic0,
                                    wavPath = wav,
                                    serial = ext?.serial?.ifBlank { game.serial.orEmpty() } ?: game.serial.orEmpty(),
                                    version = ext?.discVersion.orEmpty(),
                                    sysVersion = ext?.systemVersion.orEmpty(),
                                    region = ext?.region.orEmpty(),
                                    sizeStr = formatGameSize(sizeBytes),
                                    dateStr = formatGameDate(modifiedMs),
                                )
                                XmbAssetCache.games[cacheKey] = resolved
                                resolved
                            }
                        }
                    }.awaitAll()
                }
            }
        }
    }
    val currentAssets = rememberUpdatedState(gameAssets)

    val saveRepo = remember(context) { SaveStateRepository(context) }
    var savesRevision by remember { mutableIntStateOf(0) }
    val saveStates by produceState<List<SaveStateEntryInfo>>(initialValue = XmbAssetCache.lastSaves, savesRevision, context) {
        val enriched = withContext(Dispatchers.IO) {
            val raw = runCatching { saveRepo.listEntries() }.getOrDefault(emptyList())
            runCatching { saveRepo.enrichGlobalEntries(raw) }.getOrDefault(raw)
        }
        XmbAssetCache.lastSaves = enriched
        value = enriched
    }
    val currentSaves = rememberUpdatedState(saveStates)

    val preferences = remember(context) {
        context.getSharedPreferences("xmb_state", Context.MODE_PRIVATE)
    }
    val initialState = remember {
        intArrayOf(
            preferences.getInt("category", 5),
            preferences.getInt("item", 2),
        )
    }

    fun applyAudioAndHostSettings(
        keyTone: Boolean,
        bgmEnabled: Boolean,
        swapConfirm: Boolean,
    ) {
        XmbAudio.keyToneEnabled = keyTone
        XmbAudio.bgmEnabled = bgmEnabled
        XmbHost.swapConfirmButton = swapConfirm
        if (!bgmEnabled) {
            XmbAudio.stopBgm()
        }
    }

    fun pushSettingsToNativeOnGlThread() {
        val themeStyle = preferences.getInt("theme_style", 0)
        val colorIndex = preferences.getInt("color_index", 0)
        val bgMode = preferences.getInt("bg_mode", 0)
        val waveMode = preferences.getInt("wave_mode", 0)
        val keyTone = preferences.getBoolean("key_tone", true)
        val bgmEnabled = preferences.getBoolean("bgm_enabled", true)
        val dateFormat = preferences.getInt("date_format", 0)
        val time24h = preferences.getBoolean("time_24h", true)
        val swapConfirm = preferences.getBoolean("swap_confirm", false)
        applyAudioAndHostSettings(keyTone, bgmEnabled, swapConfirm)
        XmbNative.nativeSetSettings(
            themeStyle = themeStyle,
            colorIndex = colorIndex,
            bgMode = bgMode,
            waveMode = waveMode,
            keyTone = keyTone,
            bgmEnabled = bgmEnabled,
            dateFormat = dateFormat,
            time24h = time24h,
            swapConfirm = swapConfirm,
        )
    }

    fun saveState() {
        val state = XmbNative.nativeGetState()
        if (state.size >= 2) {
            preferences.edit()
                .putInt("category", state[0])
                .putInt("item", state[1])
                .apply()
        }
    }

    BackHandler {
        // Triggers authentic PSP exit confirmation dialog or back action
        XmbNative.nativeKey(5)
    }

    val lastAppliedAssets = remember { object { var value: List<GameAssets>? = null } }
    val lastAppliedSaves = remember { object { var value: List<SaveStateEntryInfo>? = null } }

    val surface = remember(context) {
        var downX = 0f
        var downY = 0f
        GLSurfaceView(context).apply {
            setEGLContextClientVersion(3)
            preserveEGLContextOnPause = true
            setRenderer(
                XmbSurfaceRenderer(context) {
                    pushLocalizedStrings(context)
                    pushStorageInfo()
                    pushSettingsToNativeOnGlThread()
                    pushGamesToNative(currentAssets.value)
                    pushSavesToNative(context, saveRepo, currentSaves.value)
                    XmbNative.nativeSetState(
                        preferences.getInt("category", initialState[0]),
                        preferences.getInt("item", initialState[1]),
                    )
                }
            )
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
            isFocusable = true
            isFocusableInTouchMode = true
            requestFocus()
            setOnKeyListener { _, _, event -> XmbHost.handleKeyEvent(event) }
            setOnTouchListener { _, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        downX = event.x
                        downY = event.y
                        true
                    }
                    MotionEvent.ACTION_UP -> {
                        val dx = event.x - downX
                        val dy = event.y - downY
                        val ax = abs(dx)
                        val ay = abs(dy)
                        val code = when {
                            ax < 40f && ay < 40f -> 4
                            ax > ay -> if (dx > 0) 3 else 2
                            else -> if (dy > 0) 1 else 0
                        }
                        XmbNative.nativeKey(code)
                        true
                    }
                    else -> true
                }
            }
        }
    }

    val batteryReceiver = remember {
        object : BroadcastReceiver() {
            override fun onReceive(c: Context?, intent: Intent?) {
                intent ?: return
                val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
                val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
                val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
                val charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
                    status == BatteryManager.BATTERY_STATUS_FULL
                val pct = if (scale > 0 && level >= 0) (level * 100) / scale else 100
                XmbNative.nativeSetBattery(pct, charging)
            }
        }
    }

    DisposableEffect(surface, lifecycleOwner) {
        var bgmJob: Job? = null

        applyAudioAndHostSettings(
            keyTone = preferences.getBoolean("key_tone", true),
            bgmEnabled = preferences.getBoolean("bgm_enabled", true),
            swapConfirm = preferences.getBoolean("swap_confirm", false),
        )

        val sticky = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        if (sticky != null) {
            val level = sticky.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val scale = sticky.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
            val status = sticky.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
            val charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
                status == BatteryManager.BATTERY_STATUS_FULL
            val pct = if (scale > 0 && level >= 0) (level * 100) / scale else 100
            XmbNative.nativeSetBattery(pct, charging)
        }
        context.registerReceiver(batteryReceiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))

        XmbHost.setGameListener { index ->
            if (index in currentGames.value.indices) {
                saveState()
                bgmJob?.cancel()
                XmbAudio.stopBgm()
                currentSelection.value(index)
            }
        }

        XmbHost.setFocusListener { index ->
            bgmJob?.cancel()
            val assets = currentAssets.value
            if (XmbAudio.bgmEnabled && index in assets.indices) {
                val wav = assets[index].wavPath
                if (wav.isNotEmpty()) {
                    // Authentic PSP settle delay before starting SND0 BGM preview
                    bgmJob = coroutineScope.launch {
                        delay(400)
                        if (XmbAudio.bgmEnabled) {
                            XmbAudio.playBgm(wav)
                        }
                    }
                } else {
                    XmbAudio.stopBgm()
                }
            } else {
                XmbAudio.stopBgm()
            }
        }

        XmbHost.setSaveListener { index ->
            val entry = currentSaves.value.getOrNull(index)
            if (entry != null && !entry.gamePath.isNullOrBlank()) {
                saveState()
                bgmJob?.cancel()
                XmbAudio.stopBgm()
                currentSaveSelection.value?.invoke(entry.gamePath, entry.slot)
            }
        }

        XmbHost.setDeleteSaveListener { index ->
            val entry = currentSaves.value.getOrNull(index)
            if (entry != null) {
                coroutineScope.launch(Dispatchers.IO) {
                    saveRepo.deleteEntry(entry)
                    XmbAssetCache.lastSaves = XmbAssetCache.lastSaves.filterNot {
                        it.absolutePath == entry.absolutePath
                    }
                    withContext(Dispatchers.Main) {
                        savesRevision++
                    }
                }
            }
        }

        XmbHost.setDeleteGameListener { index ->
            val game = currentGames.value.getOrNull(index)
            if (game != null) {
                currentDeleteGame.value?.invoke(game)
            }
        }

        XmbHost.setSettingsListener { arr ->
            if (arr.size >= 9) {
                val keyTone = arr[4] != 0
                val bgmEnabled = arr[5] != 0
                val time24h = arr[7] != 0
                val swapConfirm = arr[8] != 0
                preferences.edit()
                    .putInt("theme_style", arr[0])
                    .putInt("color_index", arr[1])
                    .putInt("bg_mode", arr[2])
                    .putInt("wave_mode", arr[3])
                    .putBoolean("key_tone", keyTone)
                    .putBoolean("bgm_enabled", bgmEnabled)
                    .putInt("date_format", arr[6])
                    .putBoolean("time_24h", time24h)
                    .putBoolean("swap_confirm", swapConfirm)
                    .apply()
                applyAudioAndHostSettings(keyTone, bgmEnabled, swapConfirm)
            }
        }

        XmbHost.setExitListener {
            saveState()
            bgmJob?.cancel()
            XmbAudio.stopBgm()
            currentExit.value()
        }

        XmbHost.setKeyListener { XmbNative.nativeKey(it) }

        val window = (context as? Activity)?.window
        window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> {
                    surface.onResume()
                    surface.requestFocus()
                    savesRevision++
                }
                Lifecycle.Event.ON_PAUSE -> {
                    saveState()
                    bgmJob?.cancel()
                    XmbAudio.stopBgm()
                    surface.onPause()
                }
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)

        onDispose {
            saveState()
            bgmJob?.cancel()
            XmbAudio.stopBgm()
            runCatching { context.unregisterReceiver(batteryReceiver) }
            lifecycleOwner.lifecycle.removeObserver(observer)
            XmbHost.setGameListener(null)
            XmbHost.setFocusListener(null)
            XmbHost.setSaveListener(null)
            XmbHost.setDeleteSaveListener(null)
            XmbHost.setDeleteGameListener(null)
            XmbHost.setSettingsListener(null)
            XmbHost.setExitListener(null)
            XmbHost.setKeyListener(null)
        }
    }

    LaunchedEffect(localeTag) {
        val saves = currentSaves.value
        surface.queueEvent {
            pushLocalizedStrings(context)
            pushStorageInfo()
            pushSavesToNative(context, saveRepo, saves)
        }
    }

    LaunchedEffect(gameAssets) {
        val assets = gameAssets
        if (lastAppliedAssets.value !== assets) {
            lastAppliedAssets.value = assets
            surface.queueEvent {
                pushGamesToNative(assets)
            }
        }
    }

    LaunchedEffect(saveStates) {
        val saves = saveStates
        if (lastAppliedSaves.value !== saves) {
            lastAppliedSaves.value = saves
            surface.queueEvent {
                pushSavesToNative(context, saveRepo, saves)
            }
        }
    }

    AndroidView(
        factory = { surface },
        modifier = modifier,
    )
}

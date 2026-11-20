package com.sbro.emucorea.ui.xmb

import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ActivityInfo
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.Uri
import android.opengl.GLSurfaceView
import android.os.BatteryManager
import android.os.Build
import android.os.Environment
import android.os.StatFs
import android.view.InputDevice
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
import com.sbro.emucorea.MainActivity
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

    @Volatile
    var hasLoadedSaves: Boolean = false

    @Volatile
    var returnedFromGame: Boolean = false

    fun keyFor(game: GameItem): String = "${game.path}|${game.fileSize}|${game.lastModified}"
}

private fun isNetworkAvailable(context: Context): Boolean = runCatching {
    val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
        ?: return false
    val network = cm.activeNetwork ?: return false
    val caps = cm.getNetworkCapabilities(network) ?: return false
    caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
}.getOrDefault(false)

private fun hasPhysicalGamepadConnected(): Boolean = runCatching {
    InputDevice.getDeviceIds().any { id ->
        val dev = InputDevice.getDevice(id) ?: return@any false
        if (dev.isVirtual) return@any false
        val sources = dev.sources
        val isGamepad = (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        isGamepad && dev.hasKeys(
            android.view.KeyEvent.KEYCODE_BUTTON_A,
            android.view.KeyEvent.KEYCODE_BUTTON_B,
            android.view.KeyEvent.KEYCODE_DPAD_UP,
        ).any { it }
    }
}.getOrDefault(false)

private fun pushWirelessAndAudioStatus(context: Context) {
    val wifiConnected = isNetworkAvailable(context)
    val btAdapter = runCatching {
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter
    }.getOrNull()
    val btEnabled = runCatching { btAdapter?.isEnabled == true }.getOrDefault(false)
    val audioManager = runCatching {
        context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
    }.getOrNull()
    val btAudioConnected = runCatching {
        audioManager?.getDevices(AudioManager.GET_DEVICES_OUTPUTS)?.any { dev ->
            dev.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
                dev.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO ||
                dev.type == AudioDeviceInfo.TYPE_BLE_HEADSET ||
                dev.type == AudioDeviceInfo.TYPE_BLE_SPEAKER
        } == true
    }.getOrDefault(false)
    val audioMuted = runCatching {
        (audioManager?.getStreamVolume(AudioManager.STREAM_MUSIC) ?: 1) <= 0
    }.getOrDefault(false)
    XmbNative.nativeSetWirelessStatus(
        wifiConnected = wifiConnected,
        bluetoothEnabled = btEnabled,
        bluetoothConnected = btEnabled && btAudioConnected,
        audioMuted = audioMuted,
    )
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
    "xmb_sysinfo_model" to R.string.xmb_sysinfo_model,
    "xmb_theme_cookie" to R.string.xmb_theme_cookie,
    "xmb_theme_piano" to R.string.xmb_theme_piano,
    "xmb_no_content" to R.string.xmb_no_content,
    "xmb_color_ext_00" to R.string.xmb_color_ext_00,
    "xmb_color_ext_01" to R.string.xmb_color_ext_01,
    "xmb_color_ext_02" to R.string.xmb_color_ext_02,
    "xmb_color_ext_03" to R.string.xmb_color_ext_03,
    "xmb_color_ext_04" to R.string.xmb_color_ext_04,
    "xmb_color_ext_05" to R.string.xmb_color_ext_05,
    "xmb_color_ext_06" to R.string.xmb_color_ext_06,
    "xmb_color_ext_07" to R.string.xmb_color_ext_07,
    "xmb_color_ext_08" to R.string.xmb_color_ext_08,
    "xmb_color_ext_09" to R.string.xmb_color_ext_09,
    "xmb_color_ext_10" to R.string.xmb_color_ext_10,
    "xmb_color_ext_11" to R.string.xmb_color_ext_11,
    "xmb_color_ext_12" to R.string.xmb_color_ext_12,
    "xmb_color_ext_13" to R.string.xmb_color_ext_13,
    "xmb_color_ext_14" to R.string.xmb_color_ext_14,
    "xmb_color_ext_15" to R.string.xmb_color_ext_15,
    "xmb_color_ext_16" to R.string.xmb_color_ext_16,
    "xmb_color_ext_17" to R.string.xmb_color_ext_17,
    "xmb_color_ext_18" to R.string.xmb_color_ext_18,
    "xmb_color_ext_19" to R.string.xmb_color_ext_19,
    "xmb_color_ext_20" to R.string.xmb_color_ext_20,
    "xmb_color_ext_21" to R.string.xmb_color_ext_21,
    "xmb_setting_aspect" to R.string.xmb_setting_aspect,
    "xmb_opt_16_9" to R.string.xmb_opt_16_9,
    "xmb_opt_4_3" to R.string.xmb_opt_4_3,
    "xmb_setting_seq_play" to R.string.xmb_setting_seq_play,
    "xmb_setting_ss_speed" to R.string.xmb_setting_ss_speed,
    "xmb_opt_normal" to R.string.xmb_opt_normal,
    "xmb_opt_fast" to R.string.xmb_opt_fast,
    "xmb_setting_ss_effect" to R.string.xmb_setting_ss_effect,
    "xmb_opt_slide" to R.string.xmb_opt_slide,
    "xmb_opt_fade" to R.string.xmb_opt_fade,
    "xmb_setting_restore" to R.string.xmb_setting_restore,
    "xmb_restore_confirm_1" to R.string.xmb_restore_confirm_1,
    "xmb_restore_confirm_2" to R.string.xmb_restore_confirm_2,
    "xmb_setting_backlight" to R.string.xmb_setting_backlight,
    "xmb_opt_2min" to R.string.xmb_opt_2min,
    "xmb_opt_5min" to R.string.xmb_opt_5min,
    "xmb_setting_wlan_ps" to R.string.xmb_setting_wlan_ps,
    "xmb_setting_avls" to R.string.xmb_setting_avls,
    "xmb_setting_parental" to R.string.xmb_setting_parental,
    "xmb_level_prefix" to R.string.xmb_level_prefix,
    "xmb_setting_net_status" to R.string.xmb_setting_net_status,
    "xmb_net_connected" to R.string.xmb_net_connected,
    "xmb_net_disconnected" to R.string.xmb_net_disconnected,
    "xmb_setting_adhoc_ch" to R.string.xmb_setting_adhoc_ch,
    "xmb_opt_auto" to R.string.xmb_opt_auto,
    "xmb_ch_prefix" to R.string.xmb_ch_prefix,
    "xmb_setting_rss_auto" to R.string.xmb_setting_rss_auto,
    "xmb_notice_update_1" to R.string.xmb_notice_update_1,
    "xmb_notice_update_2" to R.string.xmb_notice_update_2,
    "xmb_usb_status" to R.string.xmb_usb_status,
    "xmb_games_count_lbl" to R.string.xmb_games_count_lbl,
    "xmb_saves_count_lbl" to R.string.xmb_saves_count_lbl,
    "xmb_notice_1seg" to R.string.xmb_notice_1seg,
    "xmb_notice_camera" to R.string.xmb_notice_camera,
    "xmb_notice_gamesharing_1" to R.string.xmb_notice_gamesharing_1,
    "xmb_notice_gamesharing_2" to R.string.xmb_notice_gamesharing_2,
    "xmb_notice_manual_1" to R.string.xmb_notice_manual_1,
    "xmb_notice_manual_2" to R.string.xmb_notice_manual_2,
    "xmb_notice_manual_3" to R.string.xmb_notice_manual_3,
    "xmb_notice_manual_4" to R.string.xmb_notice_manual_4,
    "xmb_notice_locationfree" to R.string.xmb_notice_locationfree,
    "xmb_notice_remoteplay" to R.string.xmb_notice_remoteplay,
    "xmb_notice_rss" to R.string.xmb_notice_rss,
    "xmb_browser_confirm_1" to R.string.xmb_browser_confirm_1,
    "xmb_browser_confirm_2" to R.string.xmb_browser_confirm_2,
    "xmb_notice_radio" to R.string.xmb_notice_radio,
    "xmb_notice_skype" to R.string.xmb_notice_skype,
    "xmb_notice_psn" to R.string.xmb_notice_psn,
    "xmb_notice_ps_store" to R.string.xmb_notice_ps_store,
    "xmb_theme_ceramic" to R.string.xmb_theme_ceramic,
    "xmb_theme_radiant_red" to R.string.xmb_theme_radiant_red,
    "xmb_theme_vibrant_blue" to R.string.xmb_theme_vibrant_blue,
    "xmb_theme_spirited_green" to R.string.xmb_theme_spirited_green,
    "xmb_theme_bright_yellow" to R.string.xmb_theme_bright_yellow,
    "xmb_theme_blossom_pink" to R.string.xmb_theme_blossom_pink,
    "xmb_theme_turquoise" to R.string.xmb_theme_turquoise,
    "xmb_theme_lilac" to R.string.xmb_theme_lilac,
    "xmb_setting_bt_devices" to R.string.xmb_setting_bt_devices,
    "xmb_setting_bt_manage" to R.string.xmb_setting_bt_manage,
    "xmb_bt_connected" to R.string.xmb_bt_connected,
    "xmb_notice_bt_on" to R.string.xmb_notice_bt_on,
    "xmb_notice_bt_off" to R.string.xmb_notice_bt_off,
    "xmb_opt_aspect_auto" to R.string.xmb_opt_aspect_auto,
    "xmb_touch_guide_title" to R.string.xmb_touch_guide_title,
    "xmb_touch_guide_1" to R.string.xmb_touch_guide_1,
    "xmb_touch_guide_2" to R.string.xmb_touch_guide_2,
    "xmb_touch_guide_3" to R.string.xmb_touch_guide_3,
    "xmb_touch_guide_4" to R.string.xmb_touch_guide_4,
    "xmb_touch_guide_skip" to R.string.xmb_touch_guide_skip,
    "xmb_hint_options" to R.string.xmb_hint_options,
    "xmb_setting_touch_ctrl" to R.string.xmb_setting_touch_ctrl,
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
    isLoadingGames: Boolean = false,
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
    val currentLoadingGames = rememberUpdatedState(isLoadingGames)
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
        XmbAssetCache.hasLoadedSaves = true
        XmbNative.nativeSetSavesLoading(false)
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
        avlsEnabled: Boolean,
        keepScreenMode: Int,
    ) {
        XmbAudio.keyToneEnabled = keyTone
        XmbAudio.bgmEnabled = bgmEnabled
        XmbAudio.avlsEnabled = avlsEnabled
        XmbHost.swapConfirmButton = swapConfirm
        if (!bgmEnabled) {
            XmbAudio.stopBgm()
        }
        val window = (context as? Activity)?.window
        if (keepScreenMode == 0) {
            window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window?.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
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
        val avlsEnabled = preferences.getBoolean("avls_enabled", false)
        val keepScreenMode = preferences.getInt("keep_screen_mode", 0)
        val aspectMode = preferences.getInt("aspect_mode", 0)
        val slideshowSpeed = preferences.getInt("slideshow_speed", 0)
        val slideshowEffect = preferences.getInt("slideshow_effect", 0)
        val parentalLevel = preferences.getInt("parental_level", 0)
        val adhocChannel = preferences.getInt("adhoc_channel", 0)
        val rssAutoUpdate = preferences.getBoolean("rss_auto_update", false)
        val touchControlsEnabled = preferences.getBoolean("touch_controls_enabled", true)
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
            avlsEnabled = avlsEnabled,
            keepScreenMode = keepScreenMode,
            aspectMode = aspectMode,
            slideshowSpeed = slideshowSpeed,
            slideshowEffect = slideshowEffect,
            parentalLevel = parentalLevel,
            adhocChannel = adhocChannel,
            rssAutoUpdate = rssAutoUpdate,
            touchControlsEnabled = touchControlsEnabled,
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

    val activity = context as? Activity
    val mainActivity = activity as? MainActivity
    val orientationToken = remember { "xmb_${System.identityHashCode(Any())}" }
    DisposableEffect(mainActivity, orientationToken) {
        mainActivity?.setOrientationOverride(
            orientationToken,
            ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        )
        onDispose {
            mainActivity?.clearOrientationOverride(orientationToken)
        }
    }

    fun syncTouchModeToNative() {
        val touchEnabled = preferences.getBoolean("touch_controls_enabled", true)
        val touchMode = touchEnabled && !hasPhysicalGamepadConnected()
        val tutorialShown = preferences.getBoolean("touch_tutorial_shown", false)
        val shouldShowTutorial = touchMode && !tutorialShown
        if (shouldShowTutorial) {
            preferences.edit().putBoolean("touch_tutorial_shown", true).apply()
        }
        XmbNative.nativeSetTouchMode(active = touchMode, showTutorial = shouldShowTutorial)
    }

    val lastAppliedAssets = remember { object { var value: List<GameAssets>? = null } }
    val lastAppliedSaves = remember { object { var value: List<SaveStateEntryInfo>? = null } }

    val surface = remember(context) {
        var downX = 0f
        var downY = 0f
        var gestureHandled = false
        var longPressRunnable: Runnable? = null
        GLSurfaceView(context).apply {
            setEGLContextClientVersion(3)
            preserveEGLContextOnPause = true
            setRenderer(
                XmbSurfaceRenderer(context) {
                    pushLocalizedStrings(context)
                    pushStorageInfo()
                    pushWirelessAndAudioStatus(context)
                    pushSettingsToNativeOnGlThread()
                    syncTouchModeToNative()
                    val assetsSnapshot = currentAssets.value
                    val savesSnapshot = currentSaves.value
                    XmbNative.nativeSetGamesLoading(currentLoadingGames.value && assetsSnapshot.isEmpty())
                    XmbNative.nativeSetSavesLoading(!XmbAssetCache.hasLoadedSaves && savesSnapshot.isEmpty())
                    pushGamesToNative(assetsSnapshot)
                    pushSavesToNative(context, saveRepo, savesSnapshot)
                    XmbNative.nativeSetState(
                        preferences.getInt("category", initialState[0]),
                        preferences.getInt("item", initialState[1]),
                    )
                    if (XmbAssetCache.returnedFromGame) {
                        XmbAssetCache.returnedFromGame = false
                        XmbNative.nativeTriggerTransition(1)
                    } else {
                        XmbNative.nativeTriggerTransition(0)
                    }
                }
            )
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
            isFocusable = true
            isFocusableInTouchMode = true
            defaultFocusHighlightEnabled = false
            requestFocus()
            setOnKeyListener { _, _, event -> XmbHost.handleKeyEvent(event) }
            setOnTouchListener { v, event ->
                if (!preferences.getBoolean("touch_controls_enabled", true)) {
                    return@setOnTouchListener true
                }
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        downX = event.x
                        downY = event.y
                        gestureHandled = false
                        longPressRunnable?.let(v::removeCallbacks)
                        val lp = Runnable {
                            if (!gestureHandled) {
                                gestureHandled = true
                                XmbNative.nativeKey(6) // Triangle / Options menu
                            }
                        }
                        longPressRunnable = lp
                        v.postDelayed(lp, 420L)
                        true
                    }
                    MotionEvent.ACTION_POINTER_DOWN -> {
                        if (event.pointerCount >= 2 && !gestureHandled) {
                            longPressRunnable?.let(v::removeCallbacks)
                            gestureHandled = true
                            XmbNative.nativeKey(5) // Circle / Back
                        }
                        true
                    }
                    MotionEvent.ACTION_MOVE -> {
                        if (!gestureHandled && (abs(event.x - downX) > 36f || abs(event.y - downY) > 36f)) {
                            longPressRunnable?.let(v::removeCallbacks)
                        }
                        true
                    }
                    MotionEvent.ACTION_UP -> {
                        longPressRunnable?.let(v::removeCallbacks)
                        if (!gestureHandled) {
                            val dx = event.x - downX
                            val dy = event.y - downY
                            val ax = abs(dx)
                            val ay = abs(dy)
                            if (ax < 40f && ay < 40f) {
                                val normX = if (v.width > 0) (event.x / v.width.toFloat()) else 0.5f
                                val normY = if (v.height > 0) (event.y / v.height.toFloat()) else 0.5f
                                XmbNative.nativeTouchTap(normX, normY)
                            } else {
                                val code = if (ax > ay) {
                                    if (dx > 0) 2 else 3
                                } else {
                                    if (dy > 0) 0 else 1
                                }
                                XmbNative.nativeKey(code)
                            }
                        }
                        true
                    }
                    MotionEvent.ACTION_CANCEL -> {
                        longPressRunnable?.let(v::removeCallbacks)
                        gestureHandled = true
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

    val statusReceiver = remember {
        object : BroadcastReceiver() {
            override fun onReceive(c: Context?, intent: Intent?) {
                val ctx = c ?: context
                surface.queueEvent {
                    pushWirelessAndAudioStatus(ctx)
                }
            }
        }
    }

    DisposableEffect(surface, lifecycleOwner) {
        var bgmJob: Job? = null

        applyAudioAndHostSettings(
            keyTone = preferences.getBoolean("key_tone", true),
            bgmEnabled = preferences.getBoolean("bgm_enabled", true),
            swapConfirm = preferences.getBoolean("swap_confirm", false),
            avlsEnabled = preferences.getBoolean("avls_enabled", false),
            keepScreenMode = preferences.getInt("keep_screen_mode", 0),
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
        val statusFilter = IntentFilter().apply {
            addAction(BluetoothAdapter.ACTION_STATE_CHANGED)
            addAction(BluetoothAdapter.ACTION_CONNECTION_STATE_CHANGED)
            @Suppress("DEPRECATION")
            addAction(ConnectivityManager.CONNECTIVITY_ACTION)
            addAction("android.media.VOLUME_CHANGED_ACTION")
        }
        runCatching { context.registerReceiver(statusReceiver, statusFilter) }

        XmbHost.setGameListener { index ->
            if (index in currentGames.value.indices) {
                saveState()
                XmbAssetCache.returnedFromGame = true
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
                XmbAssetCache.returnedFromGame = true
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
                val avlsEnabled = arr.getOrElse(9) { 0 } != 0
                val keepScreenMode = arr.getOrElse(10) { 0 }
                val aspectMode = arr.getOrElse(11) { 0 }
                val slideshowSpeed = arr.getOrElse(12) { 0 }
                val slideshowEffect = arr.getOrElse(13) { 0 }
                val parentalLevel = arr.getOrElse(14) { 0 }
                val adhocChannel = arr.getOrElse(15) { 0 }
                val rssAutoUpdate = arr.getOrElse(16) { 0 } != 0
                val touchControlsEnabled = arr.getOrElse(17) { 1 } != 0
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
                    .putBoolean("avls_enabled", avlsEnabled)
                    .putInt("keep_screen_mode", keepScreenMode)
                    .putInt("aspect_mode", aspectMode)
                    .putInt("slideshow_speed", slideshowSpeed)
                    .putInt("slideshow_effect", slideshowEffect)
                    .putInt("parental_level", parentalLevel)
                    .putInt("adhoc_channel", adhocChannel)
                    .putBoolean("rss_auto_update", rssAutoUpdate)
                    .putBoolean("touch_controls_enabled", touchControlsEnabled)
                    .apply()
                applyAudioAndHostSettings(keyTone, bgmEnabled, swapConfirm, avlsEnabled, keepScreenMode)
            }
        }

        XmbHost.setBrowserListener { url ->
            runCatching {
                context.startActivity(
                    Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                )
            }
        }

        XmbHost.setExitListener {
            saveState()
            bgmJob?.cancel()
            XmbAudio.stopBgm()
            mainActivity?.clearOrientationOverride(orientationToken)
            currentExit.value()
        }

        XmbHost.setKeyListener { XmbNative.nativeKey(it) }

        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> {
                    mainActivity?.setOrientationOverride(
                        orientationToken,
                        ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                    )
                    surface.onResume()
                    surface.requestFocus()
                    savesRevision++
                    surface.queueEvent {
                        pushWirelessAndAudioStatus(context)
                        syncTouchModeToNative()
                    }
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
            runCatching { context.unregisterReceiver(statusReceiver) }
            lifecycleOwner.lifecycle.removeObserver(observer)
            XmbHost.setGameListener(null)
            XmbHost.setFocusListener(null)
            XmbHost.setSaveListener(null)
            XmbHost.setDeleteSaveListener(null)
            XmbHost.setDeleteGameListener(null)
            XmbHost.setSettingsListener(null)
            XmbHost.setBrowserListener(null)
            XmbHost.setExitListener(null)
            XmbHost.setKeyListener(null)
        }
    }

    LaunchedEffect(localeTag) {
        val saves = currentSaves.value
        surface.queueEvent {
            pushLocalizedStrings(context)
            pushStorageInfo()
            pushWirelessAndAudioStatus(context)
            pushSavesToNative(context, saveRepo, saves)
        }
    }

    LaunchedEffect(isLoadingGames, gameAssets) {
        val assets = gameAssets
        val loading = isLoadingGames && assets.isEmpty()
        val refChanged = lastAppliedAssets.value !== assets
        if (refChanged) {
            lastAppliedAssets.value = assets
        }
        surface.queueEvent {
            XmbNative.nativeSetGamesLoading(loading)
            if (refChanged) {
                pushGamesToNative(assets)
            }
        }
    }

    LaunchedEffect(saveStates) {
        val saves = saveStates
        val loading = !XmbAssetCache.hasLoadedSaves && saves.isEmpty()
        val refChanged = lastAppliedSaves.value !== saves
        if (refChanged) {
            lastAppliedSaves.value = saves
        }
        surface.queueEvent {
            XmbNative.nativeSetSavesLoading(loading)
            if (refChanged) {
                pushSavesToNative(context, saveRepo, saves)
            }
        }
    }

    AndroidView(
        factory = { surface },
        modifier = modifier,
    )
}

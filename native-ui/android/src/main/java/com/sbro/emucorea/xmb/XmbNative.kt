package com.sbro.emucorea.xmb

/**
 * Entry point to the native XMB renderer. The library is loaded when this
 * object is first touched; nothing here initializes the emulation core.
 */
object XmbNative {
    init {
        System.loadLibrary("emucorea_xmb")
    }

    external fun nativeVersion(): String

    external fun nativeSetResourceRoot(path: String)

    external fun nativeResourceRoot(): String

    external fun nativeInit(): Boolean

    external fun nativeResize(width: Int, height: Int)

    external fun nativeDraw()

    external fun nativeSetGames(
        titles: Array<String>,
        icons: Array<String>,
        pic1s: Array<String>,
        pic0s: Array<String>,
        wavs: Array<String>,
        serials: Array<String>,
        versions: Array<String>,
        sysVersions: Array<String>,
        regions: Array<String>,
        sizes: Array<String>,
        dates: Array<String>,
    )

    external fun nativeSetSaves(
        titles: Array<String>,
        slots: Array<String>,
        dates: Array<String>,
        sizes: Array<String>,
        previews: Array<String>,
        saves: Array<String>,
        games: Array<String>,
    )

    external fun nativeSetBattery(level: Int, charging: Boolean)

    external fun nativeSetStrings(keys: Array<String>, values: Array<String>)

    external fun nativeSetStorageInfo(freeSpaceText: String, deviceName: String)

    external fun nativeSetGamesLoading(loading: Boolean)

    external fun nativeSetSavesLoading(loading: Boolean)

    external fun nativeSetNetworkStatus(connected: Boolean)

    external fun nativeSetSettings(
        themeStyle: Int,
        colorIndex: Int,
        bgMode: Int,
        waveMode: Int,
        keyTone: Boolean,
        bgmEnabled: Boolean,
        dateFormat: Int,
        time24h: Boolean,
        swapConfirm: Boolean,
        avlsEnabled: Boolean,
        keepScreenMode: Int,
        aspectMode: Int,
        slideshowSpeed: Int,
        slideshowEffect: Int,
        parentalLevel: Int,
        adhocChannel: Int,
        rssAutoUpdate: Boolean,
    )

    external fun nativeGetState(): IntArray

    external fun nativeSetState(category: Int, item: Int)

    external fun nativeKey(key: Int)

    external fun nativeDecodeAt3(at3Path: String, wavPath: String): Boolean

    external fun nativeDecodeAt3Bytes(at3Bytes: ByteArray, wavPath: String): Boolean

    external fun nativeShutdown()
}

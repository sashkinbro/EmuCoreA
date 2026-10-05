package com.sbro.emucorea.core

import android.content.Context
import android.content.SharedPreferences

/**
 * Persisted overrides for the PPSSPP core option catalogue. Values are pushed
 * to the native core before every launch and kept in a dedicated
 * SharedPreferences file so the settings UI stays synchronous.
 */
object CoreOptionStore {
    // Kept from the previous implementation so existing users keep their options.
    private const val PREFS_NAME = "swanstation_core_options"
    private var prefs: SharedPreferences? = null
    private val cache = HashMap<String, String>()

    fun value(key: String): String? = cache[key]

    /** All persisted core option overrides. */
    fun persistedEntries(): Map<String, String> = HashMap(cache)

    fun set(key: String, value: String) {
        cache[key] = value
        prefs?.edit()?.putString(key, value)?.apply()
    }

    fun initialize(context: Context) {
        prefs = context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        cache.clear()
        prefs?.all?.forEach { (key, value) -> (value as? String)?.let { cache[key] = it } }
    }
}

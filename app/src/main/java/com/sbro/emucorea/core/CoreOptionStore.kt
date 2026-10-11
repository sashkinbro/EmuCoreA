package com.sbro.emucorea.core

import android.content.Context
import android.content.SharedPreferences
import android.util.AtomicFile
import java.io.File

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
    private var networkIdentity = ""

    fun value(key: String): String? = if (key in networkMacKeys) {
        networkMacOptions(cache, networkIdentity)[key]
    } else cache[key]

    /** All persisted core option overrides. */
    fun persistedEntries(): Map<String, String> = HashMap(cache).apply {
        // Runtime-only seed: never saved in portable preferences or game profiles.
        put(NETWORK_IDENTITY_KEY, networkIdentity)
    }

    /** The same full identity used at boot, including repaired game overrides. */
    fun networkMacValues(overrides: Map<String, String>): Map<String, String> {
        val defaults = networkMacOptions(cache, networkIdentity)
        val identity = networkMacKeys.joinToString("") { defaults[it].orEmpty() }
        return networkMacOptions(defaults + overrides, identity)
    }

    fun set(key: String, value: String) {
        if (key in networkMacKeys) {
            // An explicit edit starts from the address currently shown in the UI.
            cache.putAll(networkMacOptions(cache, networkIdentity))
            cache[key] = value
            prefs?.edit()?.apply {
                networkMacKeys.forEach { putString(it, cache.getValue(it)) }
            }?.apply()
            return
        }
        cache[key] = value
        prefs?.edit()?.putString(key, value)?.apply()
    }

    fun initialize(context: Context) {
        prefs = context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        cache.clear()
        prefs?.all?.forEach { (key, value) -> (value as? String)?.let { cache[key] = it } }
        val identityFile = AtomicFile(File(context.noBackupFilesDir, "psp-network-identity"))
        val savedIdentity = runCatching { identityFile.readFully().toString(Charsets.US_ASCII) }.getOrNull()
        networkIdentity = savedIdentity?.takeIf {
            validPspMac(it) && (it.take(2).toInt(16) and 3) == 0
        } ?: newPspNetworkIdentity().also { identity ->
            val output = identityFile.startWrite()
            try {
                output.write(identity.toByteArray(Charsets.US_ASCII))
                identityFile.finishWrite(output)
            } catch (error: Exception) {
                identityFile.failWrite(output)
                throw error
            }
        }
    }
}

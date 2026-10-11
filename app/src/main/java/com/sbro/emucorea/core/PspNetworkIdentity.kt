package com.sbro.emucorea.core

import java.security.SecureRandom
import java.util.Locale

internal const val NETWORK_IDENTITY_KEY = "_network_default_mac"
internal val networkMacKeys = (1..12).map { String.format(Locale.ROOT, "ppsspp_change_mac_address%02d", it) }

internal fun validPspMac(identity: String): Boolean =
    identity.length == 12 && identity.all { it.digitToIntOrNull(16) != null } &&
        identity.any { it != '0' } && (identity.take(2).toInt(16) and 1) == 0

internal fun newPspNetworkIdentity(): String {
    val bytes = ByteArray(6).also { SecureRandom().nextBytes(it) }
    // Match PPSSPP's Gran Turismo workaround: both OUI bits must be clear.
    bytes[0] = (bytes[0].toInt() and 0xFC).toByte()
    if (bytes.all { it == 0.toByte() }) bytes[5] = 1
    return bytes.joinToString("") { String.format(Locale.ROOT, "%02x", it.toInt() and 0xFF) }
}

internal fun networkMacOptions(overrides: Map<String, String>, identity: String): Map<String, String> {
    val requested = networkMacKeys.joinToString("") { overrides[it] ?: "0" }
    val effective = if (validPspMac(requested)) requested else identity
    if (!validPspMac(effective)) return emptyMap()
    return networkMacKeys.mapIndexed { index, key -> key to effective[index].toString() }.toMap()
}

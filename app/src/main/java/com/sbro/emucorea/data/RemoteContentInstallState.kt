package com.sbro.emucorea.data

import android.content.Context
import com.sbro.emucorea.core.EmulatorStorage
import org.json.JSONObject
import java.io.File

data class InstalledRemoteTexture(
    val packId: String,
    val serial: String,
    val version: String,
    val installedAt: Long
)

data class InstalledRemoteCheat(
    val packId: String,
    val serial: String,
    val crc: String,
    val installedAt: Long
)

class RemoteContentInstallState(
    context: Context,
    private val emulatorDataPath: String? = null
) {
    private val appContext = context.applicationContext
    private val stateFile = File(
        EmulatorStorage.appStateDir(appContext, emulatorDataPath),
        "remote-content.json"
    )
    private val lock = Any()

    fun installedTextures(): Map<String, InstalledRemoteTexture> = synchronized(lock) {
        val textures = readState().optJSONObject("textures") ?: return@synchronized emptyMap()
        // The pack directory lives under the configured emulator data root, so
        // an entry whose folder is gone (data root switched or pack deleted by
        // hand) must not keep showing as installed.
        val texturesRoot = runCatching {
            EmulatorStorage.texturesDir(appContext, emulatorDataPath).canonicalFile
        }.getOrNull()
        buildMap {
            textures.keys().forEach { id ->
                val value = textures.optJSONObject(id) ?: return@forEach
                val serial = value.optString("serial").trim()
                val version = value.optString("version").trim()
                if (serial.isEmpty() || version.isEmpty()) return@forEach
                if (texturesRoot != null &&
                    !File(texturesRoot, serial.replace("-", "")).isDirectory
                ) {
                    return@forEach
                }
                put(
                    id,
                    InstalledRemoteTexture(
                        packId = id,
                        serial = serial,
                        version = version,
                        installedAt = value.optLong("installedAt", 0L)
                    )
                )
            }
        }
    }

    fun installedCheats(): Map<String, InstalledRemoteCheat> = synchronized(lock) {
        val cheats = readState().optJSONObject("cheats") ?: return@synchronized emptyMap()
        buildMap {
            cheats.keys().forEach { id ->
                val value = cheats.optJSONObject(id) ?: return@forEach
                val serial = value.optString("serial").trim()
                if (serial.isNotEmpty()) {
                    put(
                        id,
                        InstalledRemoteCheat(
                            packId = id,
                            serial = serial,
                            crc = value.optString("crc").trim(),
                            installedAt = value.optLong("installedAt", 0L)
                        )
                    )
                }
            }
        }
    }

    fun recordTexture(pack: RemoteTexturePack, serial: String) =
        recordTexture(pack.id, pack.version, serial)

    fun recordTexture(packId: String, version: String, serial: String) = synchronized(lock) {
        val root = readState()
        val textures = root.optJSONObject("textures") ?: JSONObject().also { root.put("textures", it) }
        textures.put(
            packId,
            JSONObject()
                .put("serial", serial)
                .put("version", version)
                .put("installedAt", System.currentTimeMillis())
        )
        writeState(root)
    }

    fun recordCheat(pack: RemoteCheatPack, serial: String, crc: String?) = synchronized(lock) {
        val root = readState()
        val cheats = root.optJSONObject("cheats") ?: JSONObject().also { root.put("cheats", it) }
        cheats.put(
            pack.id,
            JSONObject()
                .put("serial", serial)
                .put("crc", crc.orEmpty())
                .put("installedAt", System.currentTimeMillis())
        )
        writeState(root)
    }

    fun removeTexturesForSerial(serial: String) = synchronized(lock) {
        val root = readState()
        val textures = root.optJSONObject("textures") ?: return@synchronized
        textures.keys().asSequence().toList().forEach { id ->
            if (textures.optJSONObject(id)?.optString("serial").equals(serial, ignoreCase = true)) {
                textures.remove(id)
            }
        }
        writeState(root)
    }

    fun removeCheatsForGame(packIds: Collection<String>) = synchronized(lock) {
        val root = readState()
        val cheats = root.optJSONObject("cheats") ?: return@synchronized
        packIds.forEach { cheats.remove(it) }
        writeState(root)
    }

    private fun readState(): JSONObject {
        if (!stateFile.exists()) return JSONObject()
        return runCatching { JSONObject(stateFile.readText()) }.getOrDefault(JSONObject())
    }

    private fun writeState(value: JSONObject) {
        stateFile.parentFile?.mkdirs()
        val temporary = File(stateFile.parentFile, "${stateFile.name}.tmp")
        temporary.writeText(value.toString())
        if (!temporary.renameTo(stateFile)) {
            temporary.copyTo(stateFile, overwrite = true)
            temporary.delete()
        }
    }
}

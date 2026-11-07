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

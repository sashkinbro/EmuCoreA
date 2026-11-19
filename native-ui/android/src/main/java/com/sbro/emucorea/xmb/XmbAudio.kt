package com.sbro.emucorea.xmb

import android.content.Context
import android.media.AudioAttributes
import android.media.SoundPool
import android.util.Log
import java.io.File

/**
 * Plays the shell's original navigation sounds from the installed pack.
 *
 * The files are the decoded `system_plugin.rco` sounds (44.1 kHz PCM16 WAV).
 * SoundPool is used only as a mixer and output device: it does not resample
 * the menu's own timing or synthesize anything.
 */
object XmbAudio {
    const val CURSOR = 0
    const val DECIDE = 1
    const val CANCEL = 2
    const val CATEGORY_DECIDE = 3
    const val OPTION = 4
    const val ERROR = 5

    private const val TAG = "XmbAudio"

    @Volatile
    private var soundPool: SoundPool? = null

    private val soundIds = IntArray(6)

    fun initialize(context: Context, root: File) {
        if (soundPool != null) {
            return
        }
        val attributes = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_GAME)
            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
            .build()
        val pool = SoundPool.Builder()
            .setMaxStreams(4)
            .setAudioAttributes(attributes)
            .build()
        pool.setOnLoadCompleteListener { _, sampleId, status ->
            if (status != 0) {
                Log.w(TAG, "sound $sampleId failed to load: $status")
            }
        }
        val files = arrayOf(
            "snd_cursor.wav",
            "snd_decide.wav",
            "snd_cancel.wav",
            "snd_category_decide.wav",
            "snd_option.wav",
            "snd_error.wav",
        )
        val audioDir = File(root, "audio/system_plugin")
        for (index in files.indices) {
            val file = File(audioDir, files[index])
            if (file.isFile) {
                soundIds[index] = pool.load(file.absolutePath, 1)
            }
        }
        soundPool = pool
        Log.i(TAG, "sounds loaded from ${audioDir.absolutePath}")
    }

    @JvmStatic
    fun play(sound: Int) {
        val pool = soundPool ?: return
        val id = soundIds.getOrNull(sound) ?: 0
        if (id != 0) {
            pool.play(id, 1.0f, 1.0f, 1, 0, 1.0f)
            Log.d(TAG, "play $sound")
        }
    }
}

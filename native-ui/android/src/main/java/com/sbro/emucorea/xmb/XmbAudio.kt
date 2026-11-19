package com.sbro.emucorea.xmb

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.SoundPool
import android.util.Log
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

/**
 * Plays the shell's original navigation sounds from the installed pack and streams
 * decoded game SND0 PCM WAV previews using a gapless AudioTrack loop.
 */
object XmbAudio {
    const val CURSOR = 0
    const val DECIDE = 1
    const val CANCEL = 2
    const val CATEGORY_DECIDE = 3
    const val OPTION = 4
    const val ERROR = 5
    const val SYSTEM_OK = 6
    const val SYSTEM_NG = 7
    const val GAMEBOOT = 8

    private const val TAG = "XmbAudio"

    @Volatile
    var keyToneEnabled: Boolean = true

    @Volatile
    var bgmEnabled: Boolean = true
        set(value) {
            field = value
            if (!value) {
                stopBgm()
            }
        }

    @Volatile
    var avlsEnabled: Boolean = false

    @Volatile
    private var soundPool: SoundPool? = null

    @Volatile
    private var currentBgmPath: String? = null

    private val bgmGeneration = AtomicInteger(0)
    private val bgmExecutor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "XmbBgmStreamer").apply {
            priority = Thread.NORM_PRIORITY + 1
            isDaemon = true
        }
    }

    private val soundIds = IntArray(9)

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
        val audioDir = File(root, "audio/system_plugin")
        val gamebootWav = File(audioDir, "snd_gameboot.wav")
        val gamebootAt3 = File(audioDir, "snd_gameboot.at3")
        if (!gamebootWav.isFile && gamebootAt3.isFile) {
            runCatching {
                XmbNative.nativeDecodeAt3Bytes(gamebootAt3.readBytes(), gamebootWav.absolutePath)
            }.onFailure {
                Log.w(TAG, "failed to decode snd_gameboot.at3", it)
            }
        }
        val files = arrayOf(
            "snd_cursor.wav",
            "snd_decide.wav",
            "snd_cancel.wav",
            "snd_category_decide.wav",
            "snd_option.wav",
            "snd_error.wav",
            "snd_system_ok.wav",
            "snd_system_ng.wav",
            "snd_gameboot.wav",
        )
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
        if (sound == GAMEBOOT) {
            stopBgm()
        }
        if (!keyToneEnabled && sound != GAMEBOOT) return
        val pool = soundPool ?: return
        val id = soundIds.getOrNull(sound) ?: 0
        if (id != 0) {
            val vol = if (avlsEnabled) 0.55f else 1.0f
            pool.play(id, vol, vol, 1, 0, 1.0f)
        }
    }

    fun playBgm(wavPath: String?) {
        if (!bgmEnabled || wavPath.isNullOrBlank() || !File(wavPath).isFile) {
            stopBgm()
            return
        }
        if (currentBgmPath == wavPath) {
            return
        }
        val gen = bgmGeneration.incrementAndGet()
        currentBgmPath = wavPath
        bgmExecutor.execute { streamWavLoop(wavPath, gen) }
    }

    fun stopBgm() {
        bgmGeneration.incrementAndGet()
        currentBgmPath = null
    }

    private data class WavFormat(
        val sampleRate: Int,
        val channels: Int,
        val bitsPerSample: Int,
        val dataOffset: Long,
        val dataSize: Long,
    )

    private fun parseWav(file: File): WavFormat? {
        RandomAccessFile(file, "r").use { raf ->
            val len = raf.length()
            if (len < 44) return null
            val header = ByteArray(12)
            raf.readFully(header)
            if (String(header, 0, 4, Charsets.US_ASCII) != "RIFF" ||
                String(header, 8, 4, Charsets.US_ASCII) != "WAVE"
            ) {
                return null
            }
            var channels = 0
            var sampleRate = 0
            var bitsPerSample = 0
            var dataOffset = -1L
            var dataSize = 0L
            var pos = 12L
            val chunkHdr = ByteArray(8)
            while (pos + 8 <= len) {
                raf.seek(pos)
                raf.readFully(chunkHdr)
                val id = String(chunkHdr, 0, 4, Charsets.US_ASCII)
                val size = ByteBuffer.wrap(chunkHdr, 4, 4).order(ByteOrder.LITTLE_ENDIAN).int.toLong() and 0xFFFFFFFFL
                val payloadPos = pos + 8
                if (id == "fmt " && size >= 16 && payloadPos + 16 <= len) {
                    val fmt = ByteArray(16)
                    raf.readFully(fmt)
                    val bb = ByteBuffer.wrap(fmt).order(ByteOrder.LITTLE_ENDIAN)
                    val audioFmt = bb.short.toInt() and 0xFFFF
                    if (audioFmt != 1) return null
                    channels = bb.short.toInt() and 0xFFFF
                    sampleRate = bb.int
                    bb.int // byteRate
                    bb.short // blockAlign
                    bitsPerSample = bb.short.toInt() and 0xFFFF
                } else if (id == "data") {
                    dataOffset = payloadPos
                    dataSize = minOf(size, len - payloadPos)
                    break
                }
                val padded = (size + 1L) and 1L.inv()
                pos = payloadPos + padded
            }
            if (channels !in 1..2 || sampleRate <= 0 || bitsPerSample != 16 || dataOffset < 0 || dataSize <= 0) {
                return null
            }
            return WavFormat(sampleRate, channels, bitsPerSample, dataOffset, dataSize)
        }
    }

    private fun streamWavLoop(wavPath: String, gen: Int) {
        if (bgmGeneration.get() != gen) return
        val file = File(wavPath)
        val wav = runCatching { parseWav(file) }.getOrNull()
        if (wav == null || bgmGeneration.get() != gen) {
            Log.w(TAG, "invalid WAV for BGM: $wavPath")
            return
        }

        val channelMask = if (wav.channels == 1) AudioFormat.CHANNEL_OUT_MONO else AudioFormat.CHANNEL_OUT_STEREO
        val encoding = AudioFormat.ENCODING_PCM_16BIT
        val minBuf = AudioTrack.getMinBufferSize(wav.sampleRate, channelMask, encoding)
        if (minBuf <= 0) {
            Log.w(TAG, "AudioTrack unsupported format: rate=${wav.sampleRate}, ch=${wav.channels}")
            return
        }
        val bufferSize = maxOf(minBuf * 2, 32 * 1024)
        val track = runCatching {
            AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(encoding)
                        .setSampleRate(wav.sampleRate)
                        .setChannelMask(channelMask)
                        .build()
                )
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setBufferSizeInBytes(bufferSize)
                .build()
        }.getOrNull() ?: return

        try {
            var lastAvls = avlsEnabled
            track.setVolume(if (lastAvls) 0.48f else 0.85f)
            track.play()
            Log.i(TAG, "playing BGM (AudioTrack): $wavPath")

            RandomAccessFile(file, "r").use { raf ->
                val chunkBuf = ByteArray(16 * 1024)
                val frameAlign = wav.channels * 2
                val totalAligned = wav.dataSize - (wav.dataSize % frameAlign)
                var remaining = totalAligned
                var fadeSamplesLeft = (wav.sampleRate * wav.channels * 80) / 1000 // 80ms soft fade-in
                val fadeTotal = maxOf(1, fadeSamplesLeft)

                raf.seek(wav.dataOffset)
                while (bgmGeneration.get() == gen && remaining > 0) {
                    val currentAvls = avlsEnabled
                    if (currentAvls != lastAvls) {
                        lastAvls = currentAvls
                        track.setVolume(if (lastAvls) 0.48f else 0.85f)
                    }
                    val toRead = minOf(chunkBuf.size.toLong(), remaining).toInt()
                    val read = raf.read(chunkBuf, 0, toRead)
                    if (read <= 0) {
                        raf.seek(wav.dataOffset)
                        remaining = totalAligned
                        continue
                    }
                    remaining -= read
                    if (fadeSamplesLeft > 0) {
                        var offset = 0
                        while (offset + 1 < read && fadeSamplesLeft > 0) {
                            val progress = 1.0f - (fadeSamplesLeft.toFloat() / fadeTotal.toFloat())
                            val sample = ((chunkBuf[offset].toInt() and 0xFF) or (chunkBuf[offset + 1].toInt() shl 8)).toShort()
                            val scaled = (sample * progress).toInt().coerceIn(-32768, 32767)
                            chunkBuf[offset] = (scaled and 0xFF).toByte()
                            chunkBuf[offset + 1] = ((scaled shr 8) and 0xFF).toByte()
                            offset += 2
                            fadeSamplesLeft--
                        }
                    }
                    val written = track.write(chunkBuf, 0, read, AudioTrack.WRITE_BLOCKING)
                    if (written < 0) break

                    if (remaining <= 0) {
                        // Seamless gapless loop back to PCM start
                        raf.seek(wav.dataOffset)
                        remaining = totalAligned
                    }
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "BGM stream error for $wavPath", e)
        } finally {
            runCatching {
                track.pause()
                track.flush()
                track.stop()
            }
            runCatching { track.release() }
        }
    }
}

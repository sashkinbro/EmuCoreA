package com.sbro.emucorea.core

import android.annotation.SuppressLint
import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import android.util.Log
import java.io.File
import java.io.FileWriter
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale


object CrashLogger {

    private const val TAG = "CrashLogger"
    private const val LOG_DIR = "logs"
    private const val LOG_FILE = "crash.log"
    private const val MAX_LOG_SIZE_BYTES = 5 * 1024 * 1024L  // 5 MB
    private const val KEEP_BYTES_AFTER_ROTATION = 4 * 1024 * 1024  // keep last 4 MB

    @Volatile
    private var logFile: File? = null

    @Volatile
    private var previousHandler: Thread.UncaughtExceptionHandler? = null

    private val dateFmt = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)

    fun init(context: Context) {
        logFile = resolveLogFile(context)
        installUncaughtExceptionHandler()
        writeStartupLine(context)
        writePreviousExitInfo(context)
    }

    fun logError(tag: String, message: String, throwable: Throwable? = null) {
        val body = buildString {
            append("[$tag] $message")
            if (throwable != null) {
                append("\n")
                append(throwable.stackTraceString())
            }
        }
        writeEntry("ERROR", body)
    }

    fun logInfo(tag: String, message: String) {
        writeEntry("INFO", "[$tag] $message")
    }

    fun logContext(key: String, value: Any?) {
        writeEntry("CTX", "$key = $value")
    }

    // ─── Private helpers ──────────────────────────────────────────────────────

    private fun resolveLogFile(context: Context): File? {
        return try {
            val dir = File(context.getExternalFilesDir(null) ?: context.filesDir, LOG_DIR)
            if (!dir.exists()) dir.mkdirs()
            File(dir, LOG_FILE)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to resolve log file", e)
            null
        }
    }

    private fun writeStartupLine(context: Context) {
        val appVersion = runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName
        }.getOrDefault("?")

        val device = "${Build.MANUFACTURER} ${Build.MODEL}"
        val androidVer = Build.VERSION.RELEASE
        val abi = Build.SUPPORTED_ABIS.firstOrNull() ?: "?"

        writeEntry("START", "APP START — v$appVersion | Android $androidVer | $device | $abi")
    }

    /**
     * Records how the previous run of the app ended. Java exceptions go through the uncaught
     * handler below, but a native crash, ANR or system kill never reaches Java code - without
     * this entry the log just stops mid-session and the cause is lost. Available since Android 11.
     */
    @SuppressLint("NewApi")
    private fun writePreviousExitInfo(context: Context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        val activityManager = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager ?: return
        val exits = try {
            activityManager.getHistoricalProcessExitReasons(context.packageName, 0, 1)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to read the previous exit reasons", e)
            return
        }
        val info = exits.firstOrNull() ?: return

        val reason = info.reason
        val normalExit = reason == ApplicationExitInfo.REASON_EXIT_SELF ||
            reason == ApplicationExitInfo.REASON_USER_REQUESTED ||
            reason == ApplicationExitInfo.REASON_USER_STOPPED ||
            reason == ApplicationExitInfo.REASON_UNKNOWN
        val level = if (normalExit) "INFO" else "ERROR"
        val description = info.description?.takeIf { it.isNotBlank() } ?: "-"
        writeEntry(
            level,
            "[PreviousExit] reason=${exitReasonName(reason)}($reason) status=${info.status} " +
                "importance=${info.importance} at=${dateFmt.format(Date(info.timestamp))} " +
                "description=$description"
        )
    }

    // Constants are inlined at compile time, so naming the newer ones is safe on older devices.
    @SuppressLint("InlinedApi")
    private fun exitReasonName(reason: Int): String = when (reason) {
        ApplicationExitInfo.REASON_EXIT_SELF -> "EXIT_SELF"
        ApplicationExitInfo.REASON_SIGNALED -> "SIGNALED"
        ApplicationExitInfo.REASON_LOW_MEMORY -> "LOW_MEMORY"
        ApplicationExitInfo.REASON_CRASH -> "CRASH"
        ApplicationExitInfo.REASON_CRASH_NATIVE -> "CRASH_NATIVE"
        ApplicationExitInfo.REASON_ANR -> "ANR"
        ApplicationExitInfo.REASON_INITIALIZATION_FAILURE -> "INITIALIZATION_FAILURE"
        ApplicationExitInfo.REASON_PERMISSION_CHANGE -> "PERMISSION_CHANGE"
        ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "EXCESSIVE_RESOURCE_USAGE"
        ApplicationExitInfo.REASON_USER_REQUESTED -> "USER_REQUESTED"
        ApplicationExitInfo.REASON_USER_STOPPED -> "USER_STOPPED"
        ApplicationExitInfo.REASON_DEPENDENCY_DIED -> "DEPENDENCY_DIED"
        ApplicationExitInfo.REASON_OTHER -> "OTHER"
        ApplicationExitInfo.REASON_FREEZER -> "FREEZER"
        ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE -> "PACKAGE_STATE_CHANGE"
        ApplicationExitInfo.REASON_PACKAGE_UPDATED -> "PACKAGE_UPDATED"
        else -> "UNKNOWN"
    }

    private fun installUncaughtExceptionHandler() {
        previousHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            try {
                writeEntry(
                    "CRASH",
                    "Thread: ${thread.name}\n${throwable.stackTraceString()}"
                )
            } catch (_: Exception) {
                // Don't let logger crash prevent the original crash from propagating
            }
            previousHandler?.uncaughtException(thread, throwable)
        }
    }

    private fun writeEntry(level: String, body: String) {
        val file = logFile ?: return
        val timestamp = dateFmt.format(Date())
        val line = "[$timestamp] $level: $body\n"

        try {
            rotateIfNeeded(file)
            FileWriter(file, /* append = */ true).use { it.write(line) }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to write log entry", e)
        }
    }

    private fun rotateIfNeeded(file: File) {
        if (!file.exists() || file.length() < MAX_LOG_SIZE_BYTES) return
        try {
            val content = file.readBytes()
            // Keep only the last KEEP_BYTES_AFTER_ROTATION bytes to avoid trimming mid-line chaos
            val trimStart = (content.size - KEEP_BYTES_AFTER_ROTATION).coerceAtLeast(0)
            // Find the next newline after trimStart so we start on a clean line
            var safeStart = trimStart
            while (safeStart < content.size && content[safeStart] != '\n'.code.toByte()) safeStart++
            safeStart++ // skip the newline itself

            val rotationHeader = "=== LOG ROTATED at ${dateFmt.format(Date())} (old entries trimmed) ===\n"
            file.writeBytes(rotationHeader.toByteArray() + content.copyOfRange(safeStart.coerceAtMost(content.size), content.size))
        } catch (e: Exception) {
            Log.e(TAG, "Failed to rotate log", e)
        }
    }

    private fun Throwable.stackTraceString(): String {
        val sw = StringWriter()
        printStackTrace(PrintWriter(sw))
        return sw.toString().trimEnd()
    }
}

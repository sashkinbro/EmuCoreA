package com.sbro.emucorea

import android.app.Application
import android.app.ActivityManager
import android.os.Build
import android.os.Process
import com.sbro.emucorea.core.AppAnalytics
import com.sbro.emucorea.core.AppIconManager
import com.sbro.emucorea.core.BackupSessionGate
import com.sbro.emucorea.core.CrashLogger
import com.sbro.emucorea.core.DeviceGpuInfoProvider
import com.sbro.emucorea.core.EmulatorBridge
import com.sbro.emucorea.data.AppPreferences
import com.sbro.emucorea.data.drive.DriveBackupArchive
import com.sbro.emucorea.data.drive.DriveBackupException
import com.sbro.emucorea.data.drive.DriveBackupWork
import com.sbro.emucorea.discord.DiscordIntegration
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

class EmuCoreAApp : Application() {
    internal val applicationScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onCreate() {
        super.onCreate()
        // The Discord SDK helper must not initialize the emulator-side application graph.
        val processName = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            getProcessName()
        } else {
            (getSystemService(ACTIVITY_SERVICE) as? ActivityManager)
                ?.runningAppProcesses?.firstOrNull { it.pid == Process.myPid() }?.processName
                ?: applicationInfo.processName
        }
        if (processName.endsWith(":discord")) return
        // CrashLogger must be the very first thing — it catches crashes in all subsequent init steps
        CrashLogger.init(this)
        if (DriveBackupArchive.hasPendingRecovery(this)) applicationScope.launch {
            try { BackupSessionGate.whileStopped { DriveBackupArchive(this@EmuCoreAApp).recoverPending() } }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) {
                // VM startup performs the same recovery under the gate before it can open cards.
                if ((error as? DriveBackupException)?.reason != "busy") {
                    android.util.Log.w("DriveBackup", "Pending restore recovery will be retried before VM startup")
                }
            }
        }
        AppAnalytics.initialize(this)
        AppIconManager.applyProIcon(this, AppPreferences(this).getProUnlockedSync())
        DriveBackupWork.resumePending(this)
        EmulatorBridge.initializeOnce(this)
        // Resolve the GPU model off the main thread (the GL renderer query opens
        // an EGL context). The overlay falls back to the SoC catalog until this
        // finishes, so a cold start never blocks on it.
        applicationScope.launch { DeviceGpuInfoProvider.preload(this@EmuCoreAApp) }
        DiscordIntegration.initialize(this)
    }
}

package com.sbro.emucorea

import android.app.Application
import android.os.Build
import androidx.annotation.RequiresApi
import com.sbro.emucorea.core.AppAnalytics
import com.sbro.emucorea.core.AppIconManager
import com.sbro.emucorea.core.BackupSessionGate
import com.sbro.emucorea.core.CrashLogger
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

    @RequiresApi(Build.VERSION_CODES.P)
    override fun onCreate() {
        super.onCreate()
        // The Discord SDK helper must not initialize the emulator-side application graph.
        if (getProcessName().endsWith(":discord")) return
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
        DiscordIntegration.initialize(this)
    }
}

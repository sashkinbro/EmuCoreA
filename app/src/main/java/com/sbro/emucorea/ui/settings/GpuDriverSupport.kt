package com.sbro.emucorea.ui.settings

import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import com.sbro.emucorea.core.DeviceGpuInfoProvider
import com.sbro.emucorea.core.GpuDriverCompatibility
import com.sbro.emucorea.core.MobileSocNameMapper
import com.sbro.emucorea.core.customDriverSupportedOnApi

/** Re-evaluate visibility when the background GL probe completes on an unknown SoC. */
@Composable
internal fun customGpuDriversSupported(): Boolean {
    val context = LocalContext.current
    val initial = remember(context) { DeviceGpuInfoProvider.get(context) }
    val snapshot by DeviceGpuInfoProvider.snapshot.collectAsState()
    return customDriverSupportedOnApi(android.os.Build.VERSION.SDK_INT) && GpuDriverCompatibility.resolveDeviceVendor(
        GpuDriverCompatibility.currentDeviceInfo(), MobileSocNameMapper.currentDeviceName(), snapshot ?: initial
    ).supportsCustomDrivers
}

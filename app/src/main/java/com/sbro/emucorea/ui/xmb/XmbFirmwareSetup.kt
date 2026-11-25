// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

package com.sbro.emucorea.ui.xmb

import android.content.Intent
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import com.sbro.emucorea.ui.theme.style.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import com.sbro.emucorea.ui.theme.style.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.sbro.emucorea.R
import com.sbro.emucorea.data.GameItem
import com.sbro.emucorea.xmb.XmbResources
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private sealed interface SetupState {
    data object WaitingForLibrary : SetupState
    data class Scanning(val checked: Int, val total: Int) : SetupState
    data class Installing(val source: XmbFirmwareInstaller.Source, val percent: Int) : SetupState
    data object NotFound : SetupState
    data object PickedFileHasNoUpdater : SetupState
    data class Failed(val message: String) : SetupState
    data class Unsupported(val version: String) : SetupState
}

/**
 * Shown in place of the XMB while no firmware has been installed. It scans the
 * library for a game disc with a firmware updater and installs it on its own;
 * only when there is none does it ask the user for a file.
 */
@Composable
fun XmbFirmwareSetup(
    games: List<GameItem>,
    isLoadingGames: Boolean,
    onInstalled: () -> Unit,
    onExit: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val currentInstalled by rememberUpdatedState(onInstalled)
    var state by remember { mutableStateOf<SetupState>(SetupState.WaitingForLibrary) }
    var attempt by remember { mutableIntStateOf(0) }

    // Tries the sources in order; a disc whose updater fails falls through to the next one.
    fun install(sources: List<XmbFirmwareInstaller.Source>) {
        if (sources.isEmpty()) return
        state = SetupState.Installing(sources.first(), 0)
        scope.launch {
            var lastError: Throwable? = null
            for (source in sources) {
                state = SetupState.Installing(source, 0)
                val result = withContext(Dispatchers.IO) {
                    runCatching {
                        XmbFirmwareInstaller.install(context, source) { percent ->
                            state = SetupState.Installing(source, percent)
                        }
                    }
                }
                if (result.isSuccess) {
                    currentInstalled()
                    return@launch
                }
                lastError = result.exceptionOrNull()
            }
            val error = lastError
            state = if (error is XmbResources.UnsupportedFirmwareException) {
                SetupState.Unsupported(error.firmware)
            } else {
                SetupState.Failed(error?.message ?: error?.javaClass?.simpleName.orEmpty())
            }
        }
    }

    // Automatic path: once the library is known, look for an updater and install it.
    LaunchedEffect(isLoadingGames, attempt, games.size) {
        if (state is SetupState.Installing) return@LaunchedEffect
        if (isLoadingGames) {
            state = SetupState.WaitingForLibrary
            return@LaunchedEffect
        }
        state = SetupState.Scanning(0, 0)
        val found = withContext(Dispatchers.IO) {
            XmbFirmwareInstaller.scanLibrary(context, games) { checked, total ->
                state = SetupState.Scanning(checked, total)
            }
        }
        if (found.isNotEmpty()) install(found) else state = SetupState.NotFound
    }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        runCatching {
            context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        state = SetupState.Scanning(0, 0)
        scope.launch {
            val source = withContext(Dispatchers.IO) { XmbFirmwareInstaller.probe(context, uri.toString()) }
            if (source != null) install(listOf(source)) else state = SetupState.PickedFileHasNoUpdater
        }
    }
    val pickFile = { picker.launch(arrayOf("*/*")) }

    BackHandler(enabled = state !is SetupState.Installing) { onExit() }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(Brush.verticalGradient(listOf(Color(0xFF0B1A3A), Color(0xFF050810))))
            .safeDrawingPadding(),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            modifier = Modifier
                .widthIn(max = 560.dp)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 24.dp, vertical = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Text(
                stringResource(R.string.xmb_fw_title),
                color = Color.White, fontSize = 22.sp, fontWeight = FontWeight.SemiBold,
                textAlign = TextAlign.Center,
            )
            Text(
                stringResource(R.string.xmb_fw_body),
                color = Color.White.copy(alpha = 0.78f), fontSize = 14.sp, textAlign = TextAlign.Center,
            )

            when (val current = state) {
                SetupState.WaitingForLibrary, is SetupState.Scanning -> {
                    CircularProgressIndicator(color = Color.White)
                    Text(
                        if (current is SetupState.Scanning && current.total > 0) {
                            stringResource(R.string.xmb_fw_scanning_progress, current.checked, current.total)
                        } else {
                            stringResource(R.string.xmb_fw_scanning)
                        },
                        color = Color.White.copy(alpha = 0.85f), fontSize = 14.sp, textAlign = TextAlign.Center,
                    )
                }

                is SetupState.Installing -> {
                    Text(
                        stringResource(R.string.xmb_fw_installing, current.source.version, current.source.displayName),
                        color = Color.White, fontSize = 15.sp, textAlign = TextAlign.Center,
                    )
                    LinearProgressIndicator(
                        progress = { current.percent / 100f },
                        modifier = Modifier.fillMaxWidth().height(6.dp),
                        color = Color.White,
                        trackColor = Color.White.copy(alpha = 0.2f),
                    )
                    Text(
                        stringResource(if (current.percent < 90) R.string.xmb_fw_unpacking else R.string.xmb_fw_extracting),
                        color = Color.White.copy(alpha = 0.7f), fontSize = 13.sp,
                    )
                }

                SetupState.NotFound, SetupState.PickedFileHasNoUpdater, is SetupState.Failed, is SetupState.Unsupported -> {
                    val message = when (current) {
                        SetupState.NotFound -> stringResource(R.string.xmb_fw_not_found)
                        SetupState.PickedFileHasNoUpdater -> stringResource(R.string.xmb_fw_no_updater_in_file)
                        is SetupState.Failed -> stringResource(R.string.xmb_fw_failed, current.message)
                        is SetupState.Unsupported -> stringResource(R.string.xmb_fw_unsupported, current.version)
                        else -> ""
                    }
                    Text(message, color = Color(0xFFFFD27A), fontSize = 14.sp, textAlign = TextAlign.Center)
                    Text(
                        stringResource(R.string.xmb_fw_hint),
                        color = Color.White.copy(alpha = 0.6f), fontSize = 12.sp, textAlign = TextAlign.Center,
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        OutlinedButton(onClick = onExit) { Text(stringResource(R.string.xmb_fw_back)) }
                        if (current is SetupState.Failed) {
                            OutlinedButton(onClick = { attempt++ }) { Text(stringResource(R.string.xmb_fw_retry)) }
                        }
                        Button(onClick = pickFile) { Text(stringResource(R.string.xmb_fw_choose_file)) }
                    }
                }
            }
        }
    }
}

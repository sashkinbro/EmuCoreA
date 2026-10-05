package com.sbro.emucorea.ui.memorycards

import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material.icons.rounded.CloudDownload
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.Save
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.sbro.emucorea.R
import com.sbro.emucorea.core.PpssppCoreOptions
import com.sbro.emucorea.core.CoreOptionStore
import com.sbro.emucorea.data.PspMemoryStickRepository
import com.sbro.emucorea.data.PspSavedGame
import com.sbro.emucorea.ui.common.AppAlertDialog
import com.sbro.emucorea.ui.common.ScreenTopBar
import com.sbro.emucorea.ui.common.appScreenTopPadding
import com.sbro.emucorea.ui.common.navigationBarsHorizontalPaddingValues
import com.sbro.emucorea.ui.theme.ScreenHorizontalPadding
import com.sbro.emucorea.ui.theme.neon.LocalNeonTheme
import com.sbro.emucorea.ui.theme.neon.NeonSystemBanner
import com.sbro.emucorea.ui.theme.neon.neonButtonShape
import com.sbro.emucorea.ui.theme.neon.neonChipShape
import com.sbro.emucorea.ui.theme.neon.neonShape
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun PspMemoryStickScreen(onBackClick: () -> Unit) {
    val context = LocalContext.current
    val repository = remember(context) { PspMemoryStickRepository(context) }
    val scope = rememberCoroutineScope()
    val sizeOption = remember { requireNotNull(PpssppCoreOptions.option("ppsspp_memstick_size")) }
    var selectedSize by remember { mutableStateOf(CoreOptionStore.value(sizeOption.key) ?: "16") }
    var saves by remember { mutableStateOf<List<PspSavedGame>>(emptyList()) }
    var cardExists by remember { mutableStateOf(false) }
    var isLoading by remember { mutableStateOf(true) }
    var isWorking by remember { mutableStateOf(false) }
    var showCreateDialog by remember { mutableStateOf(false) }
    val backupSuccess = stringResource(R.string.psp_memstick_backup_success)
    val backupFailure = stringResource(R.string.psp_memstick_backup_failure)
    val restoreSuccess = stringResource(R.string.psp_memstick_restore_success)
    val restoreFailure = stringResource(R.string.psp_memstick_restore_failure)
    val createSuccess = stringResource(R.string.psp_memstick_create_success)
    val createFailure = stringResource(R.string.psp_memstick_create_failure)
    val neonThemeActive = LocalNeonTheme.current

    fun refresh() {
        scope.launch {
            isLoading = true
            val result = withContext(Dispatchers.IO) {
                repository.saveDirectory.exists() to repository.saves()
            }
            cardExists = result.first
            saves = result.second
            isLoading = false
        }
    }
    LaunchedEffect(Unit) { refresh() }

    val backupLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        scope.launch {
            isWorking = true
            val result = withContext(Dispatchers.IO) { repository.backup(uri) }
            isWorking = false
            Toast.makeText(context, if (result) backupSuccess else backupFailure, Toast.LENGTH_SHORT).show()
        }
    }
    val restoreLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        scope.launch {
            isWorking = true
            val result = withContext(Dispatchers.IO) { repository.restore(uri) }
            isWorking = false
            Toast.makeText(context, if (result) restoreSuccess else restoreFailure, Toast.LENGTH_SHORT).show()
            refresh()
        }
    }

    if (showCreateDialog) {
        var pendingSize by remember { mutableStateOf(selectedSize) }
        AppAlertDialog(
            onDismissRequest = { showCreateDialog = false },
            title = { Text(stringResource(if (cardExists)
                R.string.psp_memstick_change_action else R.string.psp_memstick_create_title)) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
                    Text(stringResource(R.string.psp_memstick_create_body),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(stringResource(R.string.psp_memstick_capacity),
                        style = MaterialTheme.typography.titleSmall,
                        color = MaterialTheme.colorScheme.onSurface)
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        sizeOption.choices.forEach { choice ->
                            FilterChip(shape = neonChipShape(),
                                selected = pendingSize == choice.value,
                                onClick = { pendingSize = choice.value },
                                label = { Text(choice.label) })
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    showCreateDialog = false
                    scope.launch {
                        isWorking = true
                        val created = withContext(Dispatchers.IO) { repository.ensureMemoryStick() }
                        if (created) {
                            CoreOptionStore.set(sizeOption.key, pendingSize)
                            selectedSize = pendingSize
                        }
                        isWorking = false
                        Toast.makeText(context, if (created) createSuccess else createFailure,
                            Toast.LENGTH_SHORT).show()
                        refresh()
                    }
                }) { Text(stringResource(if (cardExists)
                    R.string.psp_memstick_change_action else R.string.psp_memstick_create_action)) }
            },
            dismissButton = {
                TextButton(onClick = { showCreateDialog = false }) {
                    Text(stringResource(android.R.string.cancel))
                }
            },
        )
    }

    val bottomInset = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
    Box(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)
        .padding(navigationBarsHorizontalPaddingValues())) {
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(start = ScreenHorizontalPadding,
                end = ScreenHorizontalPadding, bottom = 24.dp + bottomInset),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            item {
                ScreenTopBar(title = stringResource(R.string.psp_memstick_title),
                    onBackClick = onBackClick,
                    modifier = Modifier.padding(top = appScreenTopPadding(), bottom = 4.dp))
            }
            if (neonThemeActive) item { NeonSystemBanner() }
            if (!cardExists) item {
                FlowRow(modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    FilledTonalButton(shape = neonButtonShape(),
                        onClick = { showCreateDialog = true }, enabled = !isWorking,
                        colors = ButtonDefaults.filledTonalButtonColors(
                            containerColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.18f),
                            contentColor = MaterialTheme.colorScheme.primary)) {
                        Icon(Icons.Rounded.Add, contentDescription = null)
                        Spacer(Modifier.width(8.dp))
                        Text(stringResource(R.string.psp_memstick_create_action))
                    }
                }
            }
            if (isLoading || isWorking) item {
                Surface(shape = neonShape(20.dp), color = MaterialTheme.colorScheme.surface) {
                    Row(Modifier.padding(18.dp), verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp)
                        Text(stringResource(R.string.memory_card_loading),
                            Modifier.padding(start = 12.dp),
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
            if (!isLoading && cardExists) item {
                Surface(modifier = Modifier.fillMaxWidth(), shape = neonShape(20.dp),
                    tonalElevation = 1.dp, shadowElevation = 3.dp,
                    color = MaterialTheme.colorScheme.surface,
                    border = BorderStroke(1.dp,
                        MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.7f))) {
                    Column(Modifier.padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(14.dp)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Box(Modifier.size(54.dp).background(
                                MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.72f),
                                neonShape(16.dp)), contentAlignment = Alignment.Center) {
                                Icon(Icons.Rounded.Memory, contentDescription = null,
                                    tint = MaterialTheme.colorScheme.onPrimaryContainer)
                            }
                            Column(Modifier.padding(start = 14.dp),
                                verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                Text("PSP Memory Stick",
                                    style = MaterialTheme.typography.titleLarge.copy(
                                        fontWeight = FontWeight.SemiBold),
                                    color = MaterialTheme.colorScheme.onSurface)
                                Text(stringResource(R.string.psp_memstick_capacity_value,
                                    sizeOption.choices.firstOrNull { it.value == selectedSize }?.label
                                        ?: "$selectedSize GB"),
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                                Text(stringResource(R.string.psp_memstick_saves_count, saves.size),
                                    style = MaterialTheme.typography.labelMedium,
                                    color = MaterialTheme.colorScheme.primary)
                            }
                        }
                        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp),
                            verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            OutlinedButton(shape = neonButtonShape(),
                                onClick = { showCreateDialog = true }, enabled = !isWorking) {
                                Icon(Icons.Rounded.Add, contentDescription = null,
                                    modifier = Modifier.size(16.dp))
                                Spacer(Modifier.width(6.dp))
                                Text(stringResource(R.string.psp_memstick_change_action))
                            }
                            OutlinedButton(shape = neonButtonShape(),
                                onClick = { backupLauncher.launch("EmuCoreA-PSP-SAVEDATA.zip") },
                                enabled = !isWorking) {
                                Icon(Icons.Rounded.Save, contentDescription = null,
                                    modifier = Modifier.size(16.dp))
                                Spacer(Modifier.width(6.dp))
                                Text(stringResource(R.string.psp_memstick_backup))
                            }
                            OutlinedButton(shape = neonButtonShape(),
                                onClick = { restoreLauncher.launch(arrayOf("application/zip", "*/*")) },
                                enabled = !isWorking) {
                                Icon(Icons.Rounded.CloudDownload, contentDescription = null,
                                    modifier = Modifier.size(16.dp))
                                Spacer(Modifier.width(6.dp))
                                Text(stringResource(R.string.psp_memstick_restore))
                            }
                        }
                    }
                }
            }
            if (!isLoading && !cardExists) item {
                Surface(modifier = Modifier.fillMaxWidth(), shape = neonShape(22.dp),
                    color = MaterialTheme.colorScheme.surface) {
                    Column(Modifier.padding(18.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(stringResource(R.string.psp_memstick_empty_card),
                            style = MaterialTheme.typography.titleLarge.copy(
                                fontWeight = FontWeight.Bold),
                            color = MaterialTheme.colorScheme.onSurface)
                        Text(stringResource(R.string.psp_memstick_create_body),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
            if (!isLoading && cardExists) {
                item {
                    Text(stringResource(R.string.psp_memstick_saves),
                        style = MaterialTheme.typography.titleLarge,
                        color = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.padding(top = 10.dp))
                }
                if (saves.isEmpty()) item {
                    Surface(modifier = Modifier.fillMaxWidth(), shape = neonShape(20.dp),
                        tonalElevation = 1.dp, shadowElevation = 3.dp,
                        color = MaterialTheme.colorScheme.surface,
                        border = BorderStroke(1.dp,
                            MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.7f))) {
                        Column(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 32.dp),
                            horizontalAlignment = Alignment.CenterHorizontally,
                            verticalArrangement = Arrangement.spacedBy(12.dp)) {
                            Box(Modifier.size(64.dp).background(
                                MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.72f),
                                neonShape(20.dp)), contentAlignment = Alignment.Center) {
                                Icon(Icons.Rounded.Save, contentDescription = null,
                                    modifier = Modifier.size(30.dp),
                                    tint = MaterialTheme.colorScheme.onPrimaryContainer)
                            }
                            Text(stringResource(R.string.psp_memstick_empty),
                                style = MaterialTheme.typography.titleMedium.copy(
                                    fontWeight = FontWeight.SemiBold),
                                color = MaterialTheme.colorScheme.onSurface)
                        }
                    }
                }
                items(saves, key = { it.name }) { save ->
                    Surface(shape = neonShape(16.dp),
                        color = MaterialTheme.colorScheme.surface,
                        modifier = Modifier.fillMaxWidth()) {
                        Row(Modifier.fillMaxWidth().padding(16.dp),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically) {
                            Text(save.name, Modifier.weight(1f),
                                maxLines = 1, overflow = TextOverflow.Ellipsis,
                                color = MaterialTheme.colorScheme.onSurface)
                            Text("${save.bytes / 1024} KiB",
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                }
            }
        }
    }
}

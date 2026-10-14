package com.sbro.emucorea.ui.emulation

import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue

internal data class EmulationMenuGameHeader(
    val title: String,
    val subtitle: String,
    val coverPath: String?
)

@Composable
internal fun rememberEmulationMenuGameHeader(state: EmulationUiState): EmulationMenuGameHeader {
    val live = EmulationMenuGameHeader(state.currentGameTitle, state.currentGameSubtitle, state.currentGameCoverPath)
    var retained by remember { mutableStateOf(live) }
    // Shutdown clears live metadata before AnimatedVisibility finishes its exit transition.
    val displayed = if (state.showMenu) live else retained
    SideEffect { retained = displayed }
    return displayed
}

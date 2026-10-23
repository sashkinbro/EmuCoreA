package com.sbro.emucorea.ui.home

import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyGridScope
import androidx.compose.runtime.Composable
import com.sbro.emucorea.data.GameItem

internal fun LazyGridScope.homeLibraryItems(
    games: List<GameItem>,
    columns: Int,
    listView: Boolean,
    keyPrefix: String,
    row: @Composable (List<GameItem>, Boolean) -> Unit
) {
    val count = if (listView) 1 else columns.coerceAtLeast(1)
    items(
        count = (games.size + count - 1) / count,
        key = { index -> "${keyPrefix}_${games[index * count].path}" },
        contentType = { if (listView) "list" else "covers_$count" },
        span = { GridItemSpan(maxLineSpan) }
    ) { index ->
        val start = index * count
        row(games.subList(start, minOf(start + count, games.size)), index == 0)
    }
}

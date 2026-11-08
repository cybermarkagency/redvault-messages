// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.chats

import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

/** One category of res/raw/emoji_data.json: each entry is [emoji, its Unicode name]. */
@Serializable
private class EmojiCategory(val name: String, val emoji: List<List<String>>)

/**
 * The emoji list, generated from the desktop app's EmojiData.js (the web has
 * the same, in ui/src/lib/emojiData.ts). Read once, kept in memory only.
 */
private object EmojiData {
    @Volatile private var loaded: List<EmojiCategory>? = null

    suspend fun categories(context: Context): List<EmojiCategory> = loaded ?: withContext(Dispatchers.IO) {
        val text = context.resources.openRawResource(R.raw.emoji_data).bufferedReader().use { it.readText() }
        Json.decodeFromString<List<EmojiCategory>>(text).also { loaded = it }
    }

    /** Recently picked, newest first: for as long as the app runs, never stored. */
    val recent = ArrayList<String>()
}

/** Every emoji, by category and searchable by name; [onPick] gets the chosen one. */
@Composable
internal fun EmojiPickerDialog(onPick: (String) -> Unit, onDismiss: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val categories by produceState<List<EmojiCategory>?>(null) { value = runCatching { EmojiData.categories(context) }.getOrDefault(emptyList()) }
    var query by remember { mutableStateOf("") }
    // -1 is "Recent".
    var category by remember { mutableIntStateOf(if (EmojiData.recent.isEmpty()) 0 else -1) }

    val all = categories
    val shown: List<Pair<String, String>> = when {
        all == null -> emptyList()
        query.isNotBlank() -> {
            val q = query.trim().lowercase()
            all.flatMap { it.emoji }.filter { it.getOrNull(1)?.contains(q) == true }.take(200).map { it[0] to it[1] }
        }
        category < 0 -> EmojiData.recent.map { it to "" }
        else -> all.getOrNull(category)?.emoji.orEmpty().map { it[0] to it.getOrElse(1) { "" } }
    }

    Dialog(onDismissRequest = onDismiss) {
        Surface(shape = RoundedCornerShape(20.dp), color = colors.surface) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = query,
                    onValueChange = { query = it },
                    placeholder = { Text("Search emoji") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                if (query.isBlank() && all != null) {
                    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                        if (EmojiData.recent.isNotEmpty()) CategoryTab("🕘", "Recent", category == -1) { category = -1 }
                        all.forEachIndexed { index, c ->
                            CategoryTab(c.emoji.firstOrNull()?.firstOrNull() ?: "?", c.name, category == index) { category = index }
                        }
                    }
                }
                Box(Modifier.fillMaxWidth().height(300.dp)) {
                    when {
                        all == null -> Text("Loading…", color = colors.text3, modifier = Modifier.align(Alignment.Center))
                        shown.isEmpty() -> Text("No emoji found.", color = colors.text3, modifier = Modifier.align(Alignment.Center))
                        else -> LazyVerticalGrid(columns = GridCells.Adaptive(44.dp)) {
                            items(shown, key = { it.first }) { (emoji, name) ->
                                Box(
                                    Modifier
                                        .size(44.dp)
                                        .clip(RoundedCornerShape(10.dp))
                                        .clickable {
                                            EmojiData.recent.remove(emoji)
                                            EmojiData.recent.add(0, emoji)
                                            if (EmojiData.recent.size > 32) EmojiData.recent.removeAt(EmojiData.recent.lastIndex)
                                            onPick(emoji)
                                        }
                                        .semantics { contentDescription = name.ifEmpty { emoji } },
                                    contentAlignment = Alignment.Center,
                                ) { Text(emoji, fontSize = 26.sp) }
                            }
                        }
                    }
                }
                TextButton(onClick = onDismiss, modifier = Modifier.align(Alignment.End)) { Text("Cancel") }
            }
        }
    }
}

@Composable
private fun CategoryTab(emoji: String, name: String, selected: Boolean, onClick: () -> Unit) {
    Box(
        Modifier
            .size(40.dp)
            .clip(RoundedCornerShape(10.dp))
            .background(if (selected) RedvaultTheme.colors.accentSoft else Color.Transparent)
            .clickable(onClick = onClick)
            .semantics { contentDescription = name },
        contentAlignment = Alignment.Center,
    ) { Text(emoji, style = MaterialTheme.typography.titleMedium) }
}

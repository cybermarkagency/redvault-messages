// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.channels

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import android.content.Context
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelSuggestionDto
import com.redvaultsystems.redvaultmessage.data.model.ChannelPosts
import com.redvaultsystems.redvaultmessage.ui.chats.FormattedText
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.lock.LocalAppLock
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import java.time.Instant
import java.time.format.DateTimeFormatter

private fun statusLabel(status: String) = when (status) {
    "pending" -> "Waiting for the owner"
    "approved" -> "Published"
    "rejected" -> "Not published"
    else -> status
}

/** Owner: suggestions waiting, each to publish (text editable, now or scheduled) or turn down. Who sent them is never shown. [onCount] gets how many are left. */
@Composable
fun SuggestionReview(channels: ChannelSession, channelId: String, onPublished: (post: ChannelPostDto, scheduled: Boolean) -> Unit, onCount: (Int) -> Unit = {}) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var list by remember(channelId) { mutableStateOf<List<ChannelSuggestionDto>?>(null) }
    // Saveable, so an edit in progress survives turning the phone.
    var editing by rememberSaveable { mutableStateOf<String?>(null) }
    var draft by rememberSaveable(stateSaver = TextFieldValue.Saver) { mutableStateOf(TextFieldValue("")) }
    var publishAtMs by rememberSaveable { mutableStateOf<Long?>(null) }
    val publishAt: Instant? = publishAtMs?.let(Instant::ofEpochMilli)
    var picking by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }

    suspend fun load() {
        val loaded = runCatching { channels.suggestions(channelId) }.getOrNull()
        list = loaded ?: emptyList()
        if (loaded != null) onCount(loaded.size)
    }
    LaunchedEffect(channelId) { load() }

    fun run(action: suspend () -> Unit) {
        busy = true
        error = null
        scope.launch {
            try {
                action()
                load()
            } catch (e: Exception) {
                error = describeError(e, "Something went wrong. Try again.")
            } finally {
                busy = false
            }
        }
    }

    val shown = list
    if (shown == null) {
        Text("Loading…", color = colors.text3, fontSize = 14.sp)
        return
    }
    if (shown.isEmpty()) {
        Text("No suggested posts are waiting.", color = colors.text3, fontSize = 14.sp)
        return
    }
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Text(
            "From followers. Published, they appear as the channel's, marked as suggested; who sent them is never shown.",
            color = colors.text2,
            fontSize = 13.sp,
        )
        error?.let { Notice(it) }
        shown.forEach { s ->
            Column(
                Modifier.fillMaxWidth().border(1.dp, colors.border, RoundedCornerShape(14.dp)).padding(10.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                if (s.media.isNotEmpty()) MediaGallery(channels, s.media)
                if (editing == s.id) {
                    FormattedField(draft, { if (it.text.length <= ChannelPosts.MAX_POST) draft = it }, "Edit before publishing", minHeight = 90.dp)
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            publishAt?.let { "Posts at ${whenText(it)}" } ?: "Posts when you publish",
                            color = colors.text2,
                            fontSize = 13.sp,
                            modifier = Modifier.weight(1f),
                        )
                        TextButton(onClick = { picking = true }) { Text(if (publishAt == null) "Schedule" else "Change", color = colors.accent) }
                        if (publishAt != null) TextButton(onClick = { publishAtMs = null }) { Text("Now", color = colors.text2) }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        PillButton(if (publishAt != null) "Schedule" else "Publish", {
                            run {
                                val at = publishAt?.takeIf { it.isAfter(Instant.now()) }
                                val post = channels.approveSuggestion(channelId, s.id, draft.text, at?.let { DateTimeFormatter.ISO_INSTANT.format(it) })
                                editing = null
                                onPublished(post, at != null)
                            }
                        }, enabled = !busy, busy = busy, height = 40.dp)
                        PillButton("Cancel", { editing = null }, style = PillStyle.Secondary, enabled = !busy, height = 40.dp)
                    }
                } else {
                    if (s.body.isNotBlank()) FormattedText(s.body, color = colors.text, onOpenLink = {}, linkColor = colors.accent)
                    Text("Sent ${runCatching { whenText(Instant.parse(s.createdAt)) }.getOrDefault(s.createdAt)}", color = colors.text3, fontSize = 12.sp)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        PillButton("Review and publish", {
                            editing = s.id
                            draft = TextFieldValue(s.body)
                            publishAtMs = null
                        }, enabled = !busy, height = 40.dp)
                        PillButton("Turn down", { run { channels.rejectSuggestion(channelId, s.id) } }, style = PillStyle.Secondary, enabled = !busy, height = 40.dp)
                    }
                }
            }
        }
    }
    if (picking) {
        ScheduleDialog(initial = publishAt, onDismiss = { picking = false }) { at ->
            picking = false
            publishAtMs = at.toEpochMilli()
        }
    }
}

/** A follower's suggestion being written, kept by the channel screen so it survives closing the panel (see ComposerState). */
@Stable
class SuggestState internal constructor(context: Context, val scope: CoroutineScope, channels: ChannelSession, channelId: String) {
    val open = mutableStateOf(false)
    val body = mutableStateOf(TextFieldValue(""))
    /** The private link's token, for a private channel's uploads. */
    var access: String? = null
    val uploads = UploadQueue(context, scope, channels.media, ChannelPosts.MAX_MEDIA_PER_SUGGESTION) { meta ->
        channels.startSuggestionMedia(channelId, meta, access)
    }
}

@Composable
fun rememberSuggestState(channels: ChannelSession, channelId: String): SuggestState {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val state = remember(channelId) { SuggestState(context.applicationContext, scope, channels, channelId) }
    DisposableEffect(state) { onDispose { state.uploads.clear() } }
    return state
}

/**
 * A follower: suggest a post (text and up to four photos), and see what became of earlier ones. A panel that opens
 * from the read-only bar and scrolls within [modifier]'s height, so the keyboard can't push it off the screen.
 */
@Composable
fun SuggestPost(channels: ChannelSession, channel: ChannelDto, access: String?, state: SuggestState, onSent: () -> Unit, modifier: Modifier = Modifier) {
    val colors = RedvaultTheme.colors
    val appLock = LocalAppLock.current
    val scope = state.scope
    var open by state.open
    var body by state.body
    state.access = access
    var mine by remember(channel.id) { mutableStateOf<List<ChannelSuggestionDto>>(emptyList()) }
    var showMine by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val uploads = state.uploads
    val photos = rememberLauncherForActivityResult(ActivityResultContracts.PickMultipleVisualMedia(ChannelPosts.MAX_MEDIA_PER_SUGGESTION)) { uris ->
        uris.forEach { uploads.add(it, "photo") }
    }
    LaunchedEffect(channel.id) {
        mine = runCatching { channels.mySuggestions(channel.id) }.getOrDefault(emptyList())
    }

    Column(
        modifier.fillMaxWidth().background(colors.surface).verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        FormattedField(body, { if (it.text.length <= ChannelPosts.MAX_POST) body = it }, "Your post: fan art, a review, news… *bold*, _italic_ and links work", minHeight = 90.dp)
        UploadList(uploads)
        Text("The owner sees your post but not who sent it, and may edit it before publishing.", color = colors.text3, fontSize = 12.sp)
        error?.let { Notice(it) }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(
                Modifier.clip(RoundedCornerShape(16.dp)).background(colors.surface2)
                    .clickable(enabled = !uploads.full, role = Role.Button) {
                        appLock?.expectExternalReturn()
                        photos.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
                    }
                    .padding(horizontal = 12.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                DsIcon(R.drawable.ui_image, if (uploads.full) colors.text3 else colors.text2, 14.dp)
                Text("Photos", color = if (uploads.full) colors.text3 else colors.text2, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            }
            TextButton(onClick = {
                open = false
                uploads.clear()
            }) { Text("Cancel", color = colors.text2) }
            Spacer(Modifier.weight(1f))
            PillButton(if (busy) "Sending…" else "Send", {
                busy = true
                error = null
                scope.launch {
                    try {
                        val suggestion = channels.suggest(channel.id, body.text, uploads.ready, access)
                        mine = listOf(suggestion) + mine
                        body = TextFieldValue("")
                        uploads.clear()
                        open = false
                        onSent()
                    } catch (e: Exception) {
                        error = describeError(e, "Couldn't send your suggestion. Try again.")
                    } finally {
                        busy = false
                    }
                }
            }, enabled = !busy && !uploads.busy && (body.text.isNotBlank() || uploads.ready.isNotEmpty()), busy = busy, height = 48.dp)
        }
        if (mine.isNotEmpty()) {
            Text(
                (if (showMine) "▾ " else "▸ ") + "Your suggestions (${mine.size})",
                color = colors.text3,
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.clip(RoundedCornerShape(6.dp)).clickable(role = Role.Button) { showMine = !showMine }.padding(vertical = 4.dp),
            )
            if (showMine) {
                mine.forEach { s ->
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(s.body.ifBlank { "Photos" }, color = colors.text, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                        Text(statusLabel(s.status), color = colors.text3, fontSize = 12.sp)
                    }
                }
            }
        }
    }
}

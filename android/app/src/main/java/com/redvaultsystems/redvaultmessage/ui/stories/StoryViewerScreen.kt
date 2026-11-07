package com.redvaultsystems.redvaultmessage.ui.stories

import android.graphics.BitmapFactory
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.Story
import com.redvaultsystems.redvaultmessage.data.StoryInputException
import com.redvaultsystems.redvaultmessage.data.answerableStickers
import com.redvaultsystems.redvaultmessage.data.model.AnswerableSticker
import com.redvaultsystems.redvaultmessage.data.model.QuestionSticker
import com.redvaultsystems.redvaultmessage.data.model.StoryAnswer
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryMetaParser
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.RoundButton
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.io.File

/** One person's stories in the viewer: whose, and what, oldest first. */
class ViewerGroup(val userId: String, val handle: String, val avatarUrl: String?, val stories: List<Story>, val isOwn: Boolean)

/**
 * The full-screen viewer: progress bars, tap right or left to move, hold to pause, and on past the last story to the
 * next person's. Your own stories show who saw them and can be deleted. Everything is opened (keys, files) on this
 * phone: the server only handed over ciphertext.
 */
@Composable
internal fun StoryViewerScreen(
    session: StoriesSession,
    groups: List<ViewerGroup>,
    onClose: () -> Unit,
    /** Your own stories: add another (closes the viewer). */
    onCompose: (() -> Unit)? = null,
) {
    val colors = RedvaultTheme.colors
    if (groups.isEmpty()) {
        LaunchedEffect(Unit) { onClose() }
        return
    }
    var groupIndex by remember { mutableIntStateOf(0) }
    val group = groups[groupIndex.coerceIn(0, groups.lastIndex)]
    // Start on the first story not seen yet, like the web.
    var storyIndex by remember(groupIndex) { mutableIntStateOf(group.stories.indexOfFirst { !it.seen }.takeIf { it >= 0 && !group.isOwn } ?: 0) }
    val story = group.stories[storyIndex.coerceIn(0, group.stories.lastIndex)]
    val scope = rememberCoroutineScope()
    var paused by remember { mutableStateOf(false) }
    // A video's sound: on by default, off for the rest of the viewing once you turn it off.
    var muted by remember { mutableStateOf(false) }
    var progress by remember(story.id) { mutableFloatStateOf(0f) }
    var askDelete by remember { mutableStateOf(false) }
    var showViewers by remember { mutableStateOf<List<String>?>(null) }
    var toast by remember { mutableStateOf<String?>(null) }
    var asking by remember { mutableStateOf<QuestionSticker?>(null) }
    var linkToOpen by remember { mutableStateOf<String?>(null) }
    var showAnswers by remember { mutableStateOf(false) }
    var replyFocused by remember { mutableStateOf(false) }
    var reportingStory by remember { mutableStateOf(false) }
    var reportingAnswer by remember { mutableStateOf<com.redvaultsystems.redvaultmessage.data.model.InboxEntry?>(null) }
    // The clock and the video stop while a finger is down or any sheet is open.
    val holding = paused || askDelete || showViewers != null || asking != null || linkToOpen != null || showAnswers || replyFocused || reportingStory || reportingAnswer != null
    val context = LocalContext.current
    val sessionState by session.state.collectAsState()
    val interactionStates by session.interactionState.collectAsState()
    val interaction = interactionStates[story.id]
    val stickerEnv = StickerEnv(
        friends = sessionState.friends.associateBy { it.userId },
        isOwner = group.isOwn,
        interactive = sessionState.interactive,
        mine = interaction?.mine ?: emptyMap(),
        totals = interaction?.totals ?: emptyMap(),
        busy = interaction?.busy ?: emptySet(),
        onAnswer = { sticker, answer ->
            scope.launch {
                try {
                    session.answer(story, sticker, answer)
                } catch (e: StoryInputException) {
                    toast = e.message
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    toast = "Couldn't send that. Try again."
                }
            }
        },
        onAskQuestion = { asking = it },
        onOpenLink = { linkToOpen = it },
    )

    fun next() {
        when {
            storyIndex < group.stories.lastIndex -> storyIndex++
            groupIndex < groups.lastIndex -> groupIndex++
            else -> onClose()
        }
    }
    fun previous() {
        when {
            storyIndex > 0 -> storyIndex--
            groupIndex > 0 -> groupIndex--
            else -> progress = 0f
        }
    }

    LaunchedEffect(story.id) { runCatching { session.markSeen(story) } }
    LaunchedEffect(story.id) { session.openInteractions(story) }

    // The photo or video, opened on this phone.
    var failed by remember(story.id) { mutableStateOf(false) }
    val photo by produceState<ImageBitmap?>(null, story.id) {
        value = null
        if (story.kind == StoryKind.Image) {
            try {
                val bytes = session.photo(story)
                value = decodeScaled(bytes)
                if (value == null) failed = true
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                failed = true
            }
        }
    }
    val video by produceState<File?>(null, story.id) {
        value = null
        if (story.kind == StoryKind.Video) {
            try {
                value = session.video(story)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                failed = true
            }
        }
    }
    val ready = story.kind == StoryKind.Text || photo != null || video != null
    var videoPosition by remember(story.id) { mutableStateOf(0L) }

    // Stills run on a clock that stops while a finger is down; a video's progress is its own position.
    val stillMs = StoryTiming.stillMs(story.meta.stickers)
    LaunchedEffect(story.id, ready, holding) {
        if (story.kind == StoryKind.Video || !ready || holding) return@LaunchedEffect
        var last = System.nanoTime()
        while (progress < 1f) {
            delay(30)
            val now = System.nanoTime()
            progress = StoryTiming.advance(progress, now - last, stillMs)
            last = now
        }
        next()
    }
    LaunchedEffect(failed) {
        // A story that can't be opened is skipped after a moment, not stared at.
        if (failed) {
            delay(2_500)
            next()
        }
    }

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        Box(
            Modifier
                .align(Alignment.Center)
                .fillMaxWidth()
                .aspectRatio(9f / 16f, matchHeightConstraintsFirst = false)
                .pointerInput(story.id) {
                    var pressedAt = 0L
                    detectTapGestures(
                        onPress = {
                            pressedAt = System.currentTimeMillis()
                            paused = true
                            tryAwaitRelease()
                            paused = false
                        },
                        onTap = { offset ->
                            // A hold is for looking, not for moving on.
                            if (System.currentTimeMillis() - pressedAt > 350) return@detectTapGestures
                            if (offset.x < size.width * 0.33f) previous() else next()
                        },
                    )
                },
        ) {
            StoryFrame(
                meta = story.meta,
                photo = photo,
                video = video,
                paused = holding,
                muted = muted,
                stickers = stickerEnv,
                caption = if (story.kind == StoryKind.Video) StoryMetaParser.captionAt(story.meta.captions, videoPosition) else null,
                modifier = Modifier.fillMaxSize(),
                onVideoPosition = { position, duration ->
                    videoPosition = position
                    StoryTiming.videoProgress(position, duration)?.let { progress = it }
                },
                onVideoEnded = { next() },
            )
            if (!ready && !failed) CircularProgressIndicator(color = Color.White, modifier = Modifier.align(Alignment.Center))
            if (failed) {
                Text("This story couldn't be opened.", color = Color.White, textAlign = TextAlign.Center, modifier = Modifier.align(Alignment.Center).padding(32.dp))
            }
        }

        // Top: progress, who, close, over a soft shade so the words stay readable on any picture.
        Box(Modifier.align(Alignment.TopStart).fillMaxWidth().height(150.dp).background(Brush.verticalGradient(listOf(Color(0x99000000), Color.Transparent))))
        Column(Modifier.align(Alignment.TopStart).statusBarsPadding().padding(horizontal = 10.dp, vertical = 8.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp), modifier = Modifier.fillMaxWidth()) {
                group.stories.forEachIndexed { i, _ ->
                    val fill = when {
                        i < storyIndex -> 1f
                        i == storyIndex -> progress.coerceIn(0f, 1f)
                        else -> 0f
                    }
                    Box(Modifier.weight(1f).height(3.dp).clip(RoundedCornerShape(2.dp)).background(StoryStyle.track)) {
                        Box(Modifier.fillMaxSize(fraction = fill).background(Color.White))
                    }
                }
            }
            Row(Modifier.fillMaxWidth().padding(top = 10.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Avatar(avatarUrl = group.avatarUrl, seed = group.handle, size = 36.dp)
                Column(Modifier.weight(1f)) {
                    Text(if (group.isOwn) "Your story" else group.handle, color = StoryStyle.chrome, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
                    Text(age(story.createdAtMs), color = StoryStyle.chrome.copy(alpha = 0.7f), fontSize = 12.sp)
                }
                if (story.kind == StoryKind.Video) {
                    RoundButton(
                        if (muted) R.drawable.ui_speaker_off else R.drawable.ui_speaker,
                        if (muted) "Turn the sound on" else "Turn the sound off",
                        onClick = { muted = !muted },
                        size = 40.dp, container = StoryStyle.scrim, tint = Color.White,
                    )
                }
                if (!group.isOwn) RoundButton(R.drawable.ui_flag, "Report this story", onClick = { reportingStory = true }, size = 40.dp, container = StoryStyle.scrim, tint = Color.White)
                RoundButton(R.drawable.ui_close, "Close", onClick = onClose, size = 40.dp, container = StoryStyle.scrim, tint = Color.White)
            }
        }

        // Bottom: yours show who saw them; anyone's says what it is.
        Column(Modifier.align(Alignment.BottomCenter).navigationBarsPadding().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            toast?.let { Text(it, color = Color.White, modifier = Modifier.background(StoryStyle.scrim, RoundedCornerShape(10.dp)).padding(horizontal = 12.dp, vertical = 8.dp)) }
            if (group.isOwn) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                    Row(
                        Modifier
                            .weight(1f)
                            .clip(RoundedCornerShape(22.dp))
                            .background(StoryStyle.scrim)
                            .clickable {
                                scope.launch {
                                    showViewers = runCatching { session.viewers(story) }.getOrElse { toast = "Couldn't load who saw this."; null }
                                }
                            }
                            .padding(horizontal = 16.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        Text(if (story.viewCount == 1) "Seen by 1 friend" else "Seen by ${story.viewCount} friends", color = Color.White, fontSize = 14.sp)
                    }
                    if (story.answerableStickers().isNotEmpty()) {
                        PillButton("Answers", onClick = { showAnswers = true }, style = PillStyle.Secondary, height = 44.dp)
                    }
                    if (onCompose != null) RoundButton(R.drawable.ui_plus, "Add to your story", onClick = onCompose, size = 44.dp, container = StoryStyle.scrim, tint = Color.White)
                    RoundButton(R.drawable.ui_trash, "Delete this story", onClick = { askDelete = true }, size = 44.dp, container = StoryStyle.scrim, tint = Color.White)
                }
            } else if (story.allowReplies && story.expiresAtMs > System.currentTimeMillis()) {
                StoryReplyBar(
                    handle = group.handle,
                    onFocusChange = { replyFocused = it },
                    onSend = { text, reaction ->
                        scope.launch {
                            try {
                                session.reply(story, text, reaction)
                                toast = if (reaction) "Sent " + text else "Reply sent"
                            } catch (e: StoryInputException) {
                                toast = e.message
                            } catch (e: CancellationException) {
                                throw e
                            } catch (e: Exception) {
                                toast = "Couldn't send that. Try again."
                            }
                        }
                    },
                )
            }
        }
    }

    LaunchedEffect(toast) {
        if (toast != null) {
            delay(2_500)
            toast = null
        }
    }
    if (askDelete) {
        AlertDialog(
            onDismissRequest = { askDelete = false },
            containerColor = colors.surface,
            title = { Text("Delete this story?", color = colors.text) },
            text = { Text("It disappears for everyone who could see it. This can't be undone.", color = colors.text2) },
            confirmButton = {
                TextButton(onClick = {
                    askDelete = false
                    scope.launch {
                        runCatching { session.remove(story.id) }.onSuccess { if (group.stories.size <= 1) onClose() else next() }.onFailure { toast = "Couldn't delete that. Try again." }
                    }
                }) { Text("Delete", color = colors.danger) }
            },
            dismissButton = { TextButton(onClick = { askDelete = false }) { Text("Keep", color = colors.text2) } },
        )
    }
    showViewers?.let { names ->
        AlertDialog(
            onDismissRequest = { showViewers = null },
            containerColor = colors.surface,
            title = { Text("Seen by", color = colors.text) },
            text = { Text(if (names.isEmpty()) "Nobody has shared that they saw it yet. Friends who turned off sharing views aren't listed." else names.joinToString("\n"), color = colors.text2) },
            confirmButton = { TextButton(onClick = { showViewers = null }) { Text("Done", color = colors.accent) } },
        )
    }
    asking?.let { question ->
        QuestionDialog(
            question,
            onSend = { answer ->
                asking = null
                stickerEnv.onAnswer(question, answer)
            },
            onDismiss = { asking = null },
        )
    }
    linkToOpen?.let { url ->
        LinkDialog(
            url,
            onOpen = {
                linkToOpen = null
                try {
                    context.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(url)))
                } catch (e: Exception) {
                    toast = "No app on this phone can open that link."
                }
            },
            onDismiss = { linkToOpen = null },
        )
    }
    if (showAnswers) AnswersDialog(story, interaction, stickerEnv.friends, onReportAnswer = { reportingAnswer = it }, onDismiss = { showAnswers = false })
    if (reportingStory) {
        ReportDialog(
            "Report this story",
            evidenceLabel = story.meta.text?.let { "Include the story's text" },
            onSend = { reason, include ->
                reportingStory = false
                scope.launch {
                    toast = try {
                        session.reportStory(story, reason, if (include) story.meta.text else null)
                        "Reported. Thank you."
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        storyErrorText(e)
                    }
                }
            },
            onDismiss = { reportingStory = false },
        )
    }
    reportingAnswer?.let { entry ->
        val text = (entry.answer as? StoryAnswer.Question)?.text
        ReportDialog(
            "Report this answer",
            evidenceLabel = text?.let { "Include the answer's text" },
            onSend = { reason, include ->
                reportingAnswer = null
                scope.launch {
                    toast = try {
                        session.reportAnswer(story, entry, reason, if (include) text else null)
                        "Reported. Thank you."
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        storyErrorText(e)
                    }
                }
            },
            onDismiss = { reportingAnswer = null },
        )
    }
}

/** A photo decoded no bigger than the screen needs (the story's own pixels are at most 1920 on the long side). */
private fun decodeScaled(bytes: ByteArray): ImageBitmap? {
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
    if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
    var sample = 1
    while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= 2048) sample *= 2
    return BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })?.asImageBitmap()
}

/** "3h", "12m", "now": how long ago a story went up (the web's storyAge). */
internal fun age(createdAtMs: Long, now: Long = System.currentTimeMillis()): String {
    val mins = maxOf(0L, (now - createdAtMs) / 60_000)
    return when {
        mins < 1 -> "now"
        mins < 60 -> "${mins}m"
        mins < 24 * 60 -> "${mins / 60}h"
        mins < 7 * 24 * 60 -> "${mins / (24 * 60)}d"
        else -> "${mins / (7 * 24 * 60)}w"
    }
}

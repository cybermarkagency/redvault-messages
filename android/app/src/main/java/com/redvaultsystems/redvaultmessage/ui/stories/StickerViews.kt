// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.stories

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraintsScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.data.MyAnswer
import com.redvaultsystems.redvaultmessage.data.StoryFriend
import com.redvaultsystems.redvaultmessage.data.model.AnswerableSticker
import com.redvaultsystems.redvaultmessage.data.model.CountdownSticker
import com.redvaultsystems.redvaultmessage.data.model.DateSticker
import com.redvaultsystems.redvaultmessage.data.model.DateStyle
import com.redvaultsystems.redvaultmessage.data.model.LinkSticker
import com.redvaultsystems.redvaultmessage.data.model.LocationSticker
import com.redvaultsystems.redvaultmessage.data.model.MentionSticker
import com.redvaultsystems.redvaultmessage.data.model.PollSticker
import com.redvaultsystems.redvaultmessage.data.model.QuestionSticker
import com.redvaultsystems.redvaultmessage.data.model.QuizSticker
import com.redvaultsystems.redvaultmessage.data.model.ResultsVisibility
import com.redvaultsystems.redvaultmessage.data.model.SliderSticker
import com.redvaultsystems.redvaultmessage.data.model.StickerTotals
import com.redvaultsystems.redvaultmessage.data.model.StoryAnswer
import com.redvaultsystems.redvaultmessage.data.model.StorySticker
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.delay
import java.text.DateFormat
import java.util.Date
import kotlin.math.roundToInt

/** What a story's stickers need from the screen around them. */
internal class StickerEnv(
    val friends: Map<String, StoryFriend>,
    /** Your own story: you see the totals and don't answer. */
    val isOwner: Boolean,
    /** The server takes answers (STORIES_INTERACTIVE_ENABLED). Off: the stickers show, nothing can be pressed. */
    val interactive: Boolean,
    val mine: Map<String, MyAnswer>,
    val totals: Map<String, StickerTotals>,
    /** Answers being sent right now. */
    val busy: Set<String>,
    val onAnswer: (AnswerableSticker, StoryAnswer) -> Unit,
    /** A question wants typing: the viewer's clock stops and a box opens. */
    val onAskQuestion: (QuestionSticker) -> Unit,
    val onOpenLink: (String) -> Unit,
)

private val CardShape = RoundedCornerShape(16.dp)

/** Where a sticker sits: centred on (x, y) of the frame, turned and sized as the author left it. */
@Composable
internal fun BoxWithConstraintsScope.StickerPlaced(sticker: StorySticker, content: @Composable () -> Unit) {
    var size by remember { mutableStateOf(IntSize.Zero) }
    val px = with(LocalDensity.current) { maxWidth.toPx() }
    val py = with(LocalDensity.current) { maxHeight.toPx() }
    Box(
        Modifier
            .align(Alignment.TopStart)
            .offset { IntOffset((sticker.x * px - size.width / 2f).roundToInt(), (sticker.y * py - size.height / 2f).roundToInt()) }
            .onSizeChanged { size = it }
            .graphicsLayer { rotationZ = sticker.rotation.toFloat(); scaleX = sticker.scale.toFloat(); scaleY = sticker.scale.toFloat() },
    ) { content() }
}

/** One sticker, drawn as plain text and controls (never as markup from the story). */
@Composable
internal fun StickerView(sticker: StorySticker, frameWidth: Dp, env: StickerEnv) {
    when (sticker) {
        is PollSticker -> ChoiceCard(sticker, sticker.question, sticker.options, null, null, sticker.results, frameWidth, env)
        is QuizSticker -> ChoiceCard(sticker, sticker.question, sticker.options, sticker.correct, sticker.explanation, sticker.results, frameWidth, env)
        is QuestionSticker -> QuestionCard(sticker, frameWidth, env)
        is SliderSticker -> SliderCard(sticker, frameWidth, env)
        is CountdownSticker -> CountdownCard(sticker, frameWidth)
        is LinkSticker -> Pill("🔗 " + sticker.label, onClick = { env.onOpenLink(sticker.url) }, description = "Link: " + sticker.label)
        is MentionSticker -> Pill("@" + (env.friends[sticker.userId]?.handle ?: "someone"))
        is DateSticker -> Pill(formatWhen(sticker.at, sticker.style))
        is LocationSticker -> Pill("📍 " + sticker.label)
    }
}

@Composable
private fun Pill(text: String, onClick: (() -> Unit)? = null, description: String? = null) {
    Text(
        text,
        color = Color.White,
        fontSize = 15.sp,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier
            .clip(RoundedCornerShape(20.dp))
            .background(Color(0xCC171C1A))
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
            .then(if (description != null) Modifier.semantics { contentDescription = description } else Modifier)
            .padding(horizontal = 14.dp, vertical = 8.dp),
    )
}

@Composable
private fun CardFrame(frameWidth: Dp, label: String, content: @Composable () -> Unit) {
    Column(
        Modifier
            .widthIn(max = frameWidth * 0.78f)
            .widthIn(min = frameWidth * 0.5f)
            .clip(CardShape)
            .background(Color(0xE6171C1A))
            .padding(14.dp)
            .semantics { contentDescription = label },
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) { content() }
}

private fun percent(n: Int, total: Int) = if (total > 0) (n.toDouble() / total * 100).roundToInt() else 0

/** Whether a poll still takes votes (the author's chosen deadline; the server takes them until the story ends). */
internal fun isClosed(sticker: AnswerableSticker, now: Long = System.currentTimeMillis()): Boolean =
    sticker is PollSticker && sticker.closesAt != null && now >= sticker.closesAt

@Composable
private fun ChoiceCard(
    sticker: AnswerableSticker,
    title: String,
    options: List<String>,
    correct: Int?,
    explanation: String?,
    results: ResultsVisibility,
    frameWidth: Dp,
    env: StickerEnv,
) {
    val isQuiz = correct != null
    val closed = isClosed(sticker)
    val mine = env.mine[sticker.id]
    val chosen = when (val a = mine?.answer) {
        is StoryAnswer.Poll -> a.choice
        is StoryAnswer.Quiz -> a.choice
        else -> null
    }
    val totals = env.totals[sticker.id]
    // Totals go to the author, and to those who answered once the author chose to show them: nobody is nudged before voting.
    val showTotals = totals?.counts != null && totals.total > 0 && (env.isOwner || (results == ResultsVisibility.Everyone && mine != null))
    val reveal = isQuiz && (chosen != null || env.isOwner)
    val canPress = env.interactive && !env.isOwner && sticker.id !in env.busy && !closed && !(isQuiz && mine != null)
    CardFrame(frameWidth, (if (isQuiz) "Quiz: " else "Poll: ") + title) {
        Text(title, color = Color.White, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
        options.forEachIndexed { i, option ->
            val share = if (showTotals) percent(totals!!.counts!![i], totals.total) else 0
            val picked = chosen == i
            val right = reveal && correct == i
            val wrong = reveal && picked && correct != i
            val fill = when {
                right -> Color(0x6622C55E)
                wrong -> Color(0x66EF4444)
                picked -> Color(0x664CD07D)
                else -> Color(0x22FFFFFF)
            }
            Box(
                Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(10.dp))
                    .background(Color(0x22FFFFFF))
                    .clickable(enabled = canPress) { env.onAnswer(sticker, if (isQuiz) StoryAnswer.Quiz(i) else StoryAnswer.Poll(i)) },
            ) {
                if (showTotals || (reveal && totals?.counts != null)) {
                    Box(Modifier.fillMaxWidth(share / 100f).height(36.dp).background(fill))
                } else if (picked || right || wrong) {
                    Box(Modifier.fillMaxWidth().height(36.dp).background(fill))
                }
                Row(Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(option, color = Color.White, fontSize = 14.sp, modifier = Modifier.weight(1f))
                    if (showTotals) Text("$share%", color = Color.White, fontSize = 12.sp)
                }
            }
        }
        if (isQuiz && reveal && !explanation.isNullOrEmpty()) Note(explanation)
        Note(
            when {
                closed -> "Voting has ended"
                env.isOwner -> if (totals != null) "${totals.total} ${if (totals.total == 1) "answer" else "answers"}" + (if (isQuiz && totals.correct != null) " · ${totals.correct} right" else "") else "No answers yet"
                mine != null -> if (isQuiz) (if (chosen == correct) "Correct!" else "Not quite") else "Your vote is in. You can change it."
                results == ResultsVisibility.Author -> "Only the author sees the results"
                !env.interactive -> "Answers are switched off"
                else -> null
            },
        )
    }
}

@Composable
private fun Note(text: String?) {
    if (text != null) Text(text, color = Color(0xCCFFFFFF), fontSize = 12.sp)
}

@Composable
private fun QuestionCard(sticker: QuestionSticker, frameWidth: Dp, env: StickerEnv) {
    val sent = env.mine[sticker.id] != null
    val totals = env.totals[sticker.id]
    CardFrame(frameWidth, "Question: " + sticker.prompt) {
        Text(sticker.prompt, color = Color.White, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
        when {
            sent -> Note("Sent ✓" + (if (sticker.anonymous) " · your name isn't shown to the author" else ""))
            env.isOwner -> Note(if (totals != null) "${totals.total} ${if (totals.total == 1) "answer" else "answers"}" else "Answers appear in the answers list")
            else -> {
                Text(
                    if (env.interactive) "Tap to answer" else "Answers are switched off",
                    color = Color.White,
                    fontSize = 14.sp,
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(10.dp))
                        .background(Color(0x22FFFFFF))
                        .clickable(enabled = env.interactive && sticker.id !in env.busy) { env.onAskQuestion(sticker) }
                        .padding(horizontal = 10.dp, vertical = 10.dp),
                )
                if (sticker.anonymous) Note("Sent without your name. RedVault's server can still tell it was you.")
            }
        }
    }
}

@Composable
private fun SliderCard(sticker: SliderSticker, frameWidth: Dp, env: StickerEnv) {
    val mine = env.mine[sticker.id]
    val sentValue = (mine?.answer as? StoryAnswer.Slider)?.value
    var value by remember(sentValue) { mutableFloatStateOf((sentValue ?: 50).toFloat()) }
    val totals = env.totals[sticker.id]
    val showAverage = totals?.average != null && totals.total > 0 && (env.isOwner || (sticker.results == ResultsVisibility.Everyone && mine != null))
    val canSlide = env.interactive && !env.isOwner && sticker.id !in env.busy
    CardFrame(frameWidth, "Slider: " + sticker.prompt) {
        Text(sticker.prompt, color = Color.White, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Slider(
                value = value,
                onValueChange = { value = it },
                onValueChangeFinished = { if (canSlide) env.onAnswer(sticker, StoryAnswer.Slider(value.roundToInt())) },
                valueRange = 0f..100f,
                enabled = canSlide,
                modifier = Modifier.weight(1f).semantics { contentDescription = sticker.prompt + " " + value.roundToInt() + "%" },
                colors = SliderDefaults.colors(thumbColor = Color.White, activeTrackColor = Color(0xFF4CD07D), inactiveTrackColor = Color(0x44FFFFFF)),
            )
            Text(sticker.emoji, fontSize = 24.sp, textAlign = TextAlign.Center, modifier = Modifier.padding(start = 6.dp))
        }
        Note(
            when {
                env.isOwner -> if (totals != null) "${totals.total} ${if (totals.total == 1) "answer" else "answers"}" + (if (showAverage) " · average ${trimmed(totals.average!!)}%" else "") else "No answers yet"
                showAverage -> "Average ${trimmed(totals!!.average!!)}% from ${totals.total}"
                mine != null -> "Sent ✓"
                sticker.results == ResultsVisibility.Author -> "Only the author sees the results"
                !env.interactive -> "Answers are switched off"
                else -> null
            },
        )
    }
}

/** 55.5 stays "55.5", 60.0 becomes "60". */
internal fun trimmed(v: Double) = if (v == Math.floor(v)) v.toLong().toString() else v.toString()

@Composable
private fun CountdownCard(sticker: CountdownSticker, frameWidth: Dp) {
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(sticker.id) {
        while (true) {
            delay(1000)
            now = System.currentTimeMillis()
        }
    }
    CardFrame(frameWidth, "Countdown: " + sticker.title) {
        Text(sticker.title, color = Color.White, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
        Text(timeLeft(sticker.endsAt, now) ?: "It's time!", color = Color.White, fontSize = 22.sp, fontWeight = FontWeight.Bold)
    }
}

/** "2d 4h", "3h 12m", "45s": time left until [endsAt], or null once it has passed (the web's timeLeft). */
internal fun timeLeft(endsAt: Long, now: Long): String? {
    val ms = endsAt - now
    if (ms <= 0) return null
    val s = ms / 1000
    val d = s / 86_400
    val h = (s % 86_400) / 3600
    val m = (s % 3600) / 60
    return when {
        d > 0 -> "${d}d ${h}h"
        h > 0 -> "${h}h ${m}m"
        m > 0 -> "${m}m ${s % 60}s"
        else -> "${s}s"
    }
}

private fun formatWhen(at: Long, style: DateStyle): String {
    val d = Date(at)
    val date = DateFormat.getDateInstance(DateFormat.MEDIUM).format(d)
    val time = DateFormat.getTimeInstance(DateFormat.SHORT).format(d)
    return when (style) {
        DateStyle.Date -> date
        DateStyle.Time -> time
        DateStyle.DateTime -> "$date · $time"
    }
}

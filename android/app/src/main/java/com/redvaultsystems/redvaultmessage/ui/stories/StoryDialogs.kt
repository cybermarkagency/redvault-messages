package com.redvaultsystems.redvaultmessage.ui.stories

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.data.Story
import com.redvaultsystems.redvaultmessage.data.StoryFriend
import com.redvaultsystems.redvaultmessage.data.answerableStickers
import com.redvaultsystems.redvaultmessage.data.model.PollSticker
import com.redvaultsystems.redvaultmessage.data.model.QuestionSticker
import com.redvaultsystems.redvaultmessage.data.model.QuizSticker
import com.redvaultsystems.redvaultmessage.data.model.SliderSticker
import com.redvaultsystems.redvaultmessage.data.model.StickerLimits
import com.redvaultsystems.redvaultmessage.data.model.StoryAnswer
import com.redvaultsystems.redvaultmessage.data.model.jsTrim
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull

/** Typing the answer to a question sticker. A question sent "without your name" says what that does and doesn't hide. */
@Composable
internal fun QuestionDialog(sticker: QuestionSticker, onSend: (StoryAnswer.Question) -> Unit, onDismiss: () -> Unit) {
    val colors = RedvaultTheme.colors
    var text by remember { mutableStateOf("") }
    val cleaned = jsTrim(text)
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = colors.surface,
        title = { Text(sticker.prompt, color = colors.text, fontSize = 17.sp) },
        text = {
            Column {
                TextField(
                    value = text,
                    onValueChange = { if (it.length <= StickerLimits.MAX_QUESTION_ANSWER) text = it },
                    placeholder = { Text("Type your answer", color = colors.placeholder) },
                    colors = TextFieldDefaults.colors(
                        focusedContainerColor = colors.surface2,
                        unfocusedContainerColor = colors.surface2,
                        focusedTextColor = colors.text,
                        unfocusedTextColor = colors.text,
                    ),
                )
                if (sticker.anonymous) {
                    Text(
                        "Sent without your name. RedVault's server can still tell it was you.",
                        color = colors.text3,
                        fontSize = 12.sp,
                        modifier = Modifier.padding(top = 8.dp),
                    )
                }
            }
        },
        confirmButton = { TextButton(enabled = cleaned.isNotEmpty(), onClick = { onSend(StoryAnswer.Question(cleaned)) }) { Text("Send", color = colors.accent) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel", color = colors.text2) } },
    )
}

/** Leaving the app for a link: say where it goes first, and that the browser will learn this phone's address. */
@Composable
internal fun LinkDialog(url: String, onOpen: () -> Unit, onDismiss: () -> Unit) {
    val colors = RedvaultTheme.colors
    val host = url.toHttpUrlOrNull()?.host ?: url
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = colors.surface,
        title = { Text("Open $host?", color = colors.text) },
        text = {
            Column {
                Text(url, color = colors.text2, fontSize = 13.sp)
                Text(
                    "It opens in your browser. The site, and your browser, will see this phone's address.",
                    color = colors.text3,
                    fontSize = 12.sp,
                    modifier = Modifier.padding(top = 8.dp),
                )
            }
        },
        confirmButton = { TextButton(onClick = onOpen) { Text("Open", color = colors.accent) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Stay here", color = colors.text2) } },
    )
}

/**
 * Reporting a story, or an answer: why, and optionally what it shows. Nothing from the story is sent unless the box is ticked, and the box
 * says exactly what would go.
 */
@Composable
internal fun ReportDialog(title: String, evidenceLabel: String?, onSend: (reason: String, includeEvidence: Boolean) -> Unit, onDismiss: () -> Unit) {
    val colors = RedvaultTheme.colors
    var reason by remember { mutableStateOf("") }
    var include by remember { mutableStateOf(false) }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = colors.surface,
        title = { Text(title, color = colors.text, fontSize = 17.sp) },
        text = {
            Column {
                TextField(
                    value = reason,
                    onValueChange = { if (it.length <= 300) reason = it },
                    placeholder = { Text("What is wrong?", color = colors.placeholder) },
                    colors = TextFieldDefaults.colors(focusedContainerColor = colors.surface2, unfocusedContainerColor = colors.surface2, focusedTextColor = colors.text, unfocusedTextColor = colors.text),
                )
                if (evidenceLabel != null) {
                    androidx.compose.foundation.layout.Row(
                        Modifier.padding(top = 8.dp).clickable { include = !include },
                        verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
                    ) {
                        androidx.compose.material3.Checkbox(
                            checked = include,
                            onCheckedChange = null,
                            colors = androidx.compose.material3.CheckboxDefaults.colors(checkedColor = colors.accent, checkmarkColor = colors.accentContrast, uncheckedColor = colors.text3),
                        )
                        Text(evidenceLabel, color = colors.text2, fontSize = 13.sp, modifier = Modifier.padding(start = 8.dp))
                    }
                }
                Text(
                    "Only what you type, and what you tick, goes to the moderators. Nothing else from the story is sent.",
                    color = colors.text3,
                    fontSize = 12.sp,
                    modifier = Modifier.padding(top = 8.dp),
                )
            }
        },
        confirmButton = { TextButton(enabled = reason.isNotBlank(), onClick = { onSend(reason, include) }) { Text("Report", color = colors.danger) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel", color = colors.text2) } },
    )
}

/** Your own story's answers: the totals, and what people wrote (with a name, or "Anonymous" when they asked for that). */
@Composable
internal fun AnswersDialog(
    story: Story,
    state: StoryInteractionState?,
    friends: Map<String, StoryFriend>,
    onReportAnswer: (com.redvaultsystems.redvaultmessage.data.model.InboxEntry) -> Unit,
    onDismiss: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    val inbox = state?.inbox
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = colors.surface,
        title = { Text("Answers", color = colors.text) },
        text = {
            Column(Modifier.heightIn(max = 420.dp).verticalScroll(rememberScrollState())) {
                if (inbox == null) {
                    Text("Loading the answers…", color = colors.text2)
                } else {
                    for (sticker in story.answerableStickers()) {
                        val totals = state.totals[sticker.id]
                        when (sticker) {
                            is PollSticker -> Section(sticker.question, sticker.options.mapIndexed { i, o -> o + ": " + (totals?.counts?.get(i) ?: 0) })
                            is QuizSticker -> Section(
                                sticker.question,
                                sticker.options.mapIndexed { i, o -> o + (if (i == sticker.correct) " (right)" else "") + ": " + (totals?.counts?.get(i) ?: 0) },
                            )
                            is SliderSticker -> Section(sticker.prompt, listOf(if (totals == null || totals.total == 0) "No answers yet" else "Average ${trimmed(totals.average ?: 0.0)}% from ${totals.total}"))
                            is QuestionSticker -> {
                                val written = inbox.entries.filter { it.sticker.id == sticker.id }
                                Section(sticker.prompt, if (written.isEmpty()) listOf("No answers yet") else emptyList())
                                for (e in written) {
                                    androidx.compose.foundation.layout.Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                                        Text(
                                            (e.responderId?.let { friends[it]?.handle ?: "Former friend" } ?: "Anonymous") + ": " + ((e.answer as? StoryAnswer.Question)?.text ?: ""),
                                            color = colors.text2,
                                            fontSize = 13.sp,
                                            modifier = Modifier.weight(1f),
                                        )
                                        Text("Report", color = colors.danger, fontSize = 12.sp, modifier = Modifier.clickable { onReportAnswer(e) }.padding(8.dp))
                                    }
                                }
                            }
                        }
                    }
                    if (inbox.unreadable > 0) {
                        Text(
                            "${inbox.unreadable} answer${if (inbox.unreadable == 1) "" else "s"} couldn't be opened (for instance from someone who isn't your friend any more) and aren't counted.",
                            color = colors.text3,
                            fontSize = 12.sp,
                            modifier = Modifier.padding(top = 8.dp),
                        )
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Done", color = colors.accent) } },
    )
}

@Composable
private fun Section(title: String, lines: List<String>) {
    val colors = RedvaultTheme.colors
    Text(title, color = colors.text, fontWeight = FontWeight.SemiBold, fontSize = 14.sp, modifier = Modifier.padding(top = 10.dp))
    for (line in lines) Text(line, color = colors.text2, fontSize = 13.sp)
}

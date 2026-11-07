package com.redvaultsystems.redvaultmessage.ui.chats

import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.LinkAnnotation
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.withLink
import androidx.compose.ui.text.withStyle
import com.redvaultsystems.redvaultmessage.data.model.FormatNode
import com.redvaultsystems.redvaultmessage.data.model.FormatStyle
import com.redvaultsystems.redvaultmessage.data.model.TextFormat

/**
 * A message's text with its formatting (data/model/TextFormat.kt): the
 * untrusted text is only ever appended as text. Links aren't opened here:
 * [onOpenLink] gets the address so the screen can ask first. Tapping a
 * spoiler reveals the message's spoilers.
 */
@Composable
internal fun FormattedText(
    text: String,
    color: Color,
    onOpenLink: (String) -> Unit,
    modifier: Modifier = Modifier,
    /** Handles to highlight as "@handle" (a group's members). */
    mentions: List<String> = emptyList(),
    selfHandle: String? = null,
    linkColor: Color = color,
    style: TextStyle = LocalTextStyle.current,
) {
    var revealed by remember(text) { mutableStateOf(false) }
    val nodes = remember(text, mentions, selfHandle) { TextFormat.parse(text, mentions, selfHandle) }
    if (nodes.all { it is FormatNode.Plain }) {
        Text(text, color = color, modifier = modifier, style = style)
        return
    }
    val annotated = remember(nodes, revealed, color, linkColor) {
        buildAnnotatedString {
            appendNodes(nodes, color, linkColor, revealed, onReveal = { revealed = true }, onOpenLink)
        }
    }
    Text(annotated, color = color, modifier = modifier, style = style)
}

private fun AnnotatedString.Builder.appendNodes(
    nodes: List<FormatNode>,
    color: Color,
    linkColor: Color,
    revealed: Boolean,
    onReveal: () -> Unit,
    onOpenLink: (String) -> Unit,
) {
    for (node in nodes) {
        when (node) {
            is FormatNode.Plain -> append(node.text)
            is FormatNode.Code -> withStyle(SpanStyle(fontFamily = FontFamily.Monospace, background = color.copy(alpha = 0.14f))) { append(node.text) }
            is FormatNode.Mention -> withStyle(
                SpanStyle(
                    fontWeight = FontWeight.SemiBold,
                    color = linkColor,
                    background = if (node.self) linkColor.copy(alpha = 0.18f) else Color.Unspecified,
                ),
            ) { append(node.text) }
            is FormatNode.Link -> withLink(
                LinkAnnotation.Clickable(
                    tag = "link",
                    styles = TextLinkStyles(SpanStyle(color = linkColor, textDecoration = TextDecoration.Underline)),
                ) { onOpenLink(node.url) },
            ) { append(node.url) }
            is FormatNode.Styled -> when (node.style) {
                FormatStyle.Bold -> withStyle(SpanStyle(fontWeight = FontWeight.Bold)) { appendNodes(node.children, color, linkColor, revealed, onReveal, onOpenLink) }
                FormatStyle.Italic -> withStyle(SpanStyle(fontStyle = FontStyle.Italic)) { appendNodes(node.children, color, linkColor, revealed, onReveal, onOpenLink) }
                FormatStyle.Strike -> withStyle(SpanStyle(textDecoration = TextDecoration.LineThrough)) {
                    appendNodes(node.children, color, linkColor, revealed, onReveal, onOpenLink)
                }
                FormatStyle.Spoiler -> {
                    val body = (node.children.firstOrNull() as? FormatNode.Plain)?.text.orEmpty()
                    if (revealed) {
                        withStyle(SpanStyle(background = color.copy(alpha = 0.1f))) { append(body) }
                    } else {
                        // Hidden content isn't laid out at all, so it can't be read
                        // off the screen or by accessibility services until tapped.
                        withLink(
                            LinkAnnotation.Clickable(
                                tag = "spoiler",
                                styles = TextLinkStyles(SpanStyle(color = color.copy(alpha = 0.35f), background = color.copy(alpha = 0.35f))),
                            ) { onReveal() },
                        ) { append("•".repeat(body.length.coerceIn(3, 12))) }
                    }
                }
            }
        }
    }
}

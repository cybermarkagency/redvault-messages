package com.redvaultsystems.redvaultmessage.data.model

// Port of ui/src/lib/textFormat.ts (and desktop/core/.../model/text_format.hpp):
// WhatsApp-style markers inside the ordinary text, so an app that doesn't
// render them still shows something readable.
//
//   *bold*   _italic_   ~strikethrough~   `monospace`   ||spoiler||
//
// plus http(s) links, and "@handle" mentions of a group's members. A marker
// only opens after the start of a line, a space or an opening bracket/quote
// and only closes before the end, a space or punctuation, so snake_case,
// 2*3*4 and file_names stay as typed. Nothing inside `monospace` is
// formatted, and markers never span lines.
//
// The result is a tree of nodes, never markup: the text is untrusted, and
// the renderer (ui/chats/FormattedText.kt) only ever appends it as text.

sealed interface FormatNode {
    data class Plain(val text: String) : FormatNode
    data class Styled(val style: FormatStyle, val children: List<FormatNode>) : FormatNode
    data class Code(val text: String) : FormatNode
    data class Link(val url: String) : FormatNode
    data class Mention(val text: String, val self: Boolean) : FormatNode
}

enum class FormatStyle { Bold, Italic, Strike, Spoiler }

object TextFormat {
    // Messages are untrusted: a formatted span is at most this long, and
    // styles nest at most this deep (deeper markers are shown as typed).
    private const val MAX_SPAN = 4000
    private const val MAX_DEPTH = 6

    // "||" before the one-character markers, so a spoiler isn't read as two empty ones.
    private val MARKERS = listOf("||" to FormatStyle.Spoiler, "*" to FormatStyle.Bold, "_" to FormatStyle.Italic, "~" to FormatStyle.Strike)

    private fun isSpace(c: Char) = c == ' ' || c == '\t' || c == '\n' || c == '\r'

    /** Letters, digits and anything non-ASCII (part of a word in another script). */
    private fun isWordChar(c: Char) = c.code >= 0x80 || c in 'A'..'Z' || c in 'a'..'z' || c in '0'..'9'
    private fun canOpenAfter(s: String, i: Int) = i == 0 || isSpace(s[i - 1]) || s[i - 1] in "([{\"'"
    private fun canCloseBefore(s: String, j: Int) = j >= s.length || isSpace(s[j]) || !isWordChar(s[j])

    /** End of an http(s) link starting at [i], or [i] if there's none. */
    private fun linkEnd(s: String, i: Int): Int {
        if (i > 0 && isWordChar(s[i - 1])) return i
        val lower = s.substring(i, minOf(s.length, i + 8)).lowercase()
        val scheme = when {
            lower.startsWith("https://") -> 8
            lower.startsWith("http://") -> 7
            else -> return i
        }
        var end = i + scheme
        while (end < s.length && !isSpace(s[end]) && s[end] !in "<>\"`") end++
        // Sentence punctuation and formatting markers right after a link aren't part of it.
        var open = 0
        for (k in i until end) open += if (s[k] == '(') 1 else if (s[k] == ')') -1 else 0
        while (end > i + scheme) {
            val last = s[end - 1]
            if (last == ')' && open < 0) {
                open++
                end--
            } else if (last in ".,;:!?'*_~|") {
                end--
            } else {
                break
            }
        }
        return if (end > i + scheme) end else i
    }

    private fun findClose(s: String, i: Int, token: String): Int {
        val limit = minOf(s.length, i + token.length + MAX_SPAN)
        var j = i + token.length + 1
        while (j + token.length <= limit) {
            if (s[j] == '\n') return -1
            if (s.startsWith(token, j) && !isSpace(s[j - 1]) && canCloseBefore(s, j + token.length)) return j
            j++
        }
        return -1
    }

    /** The handle the "@" at [i] mentions (the longest that fits, as typed), or null. */
    private fun mentionAt(s: String, i: Int, handles: List<String>): String? {
        if (s[i] != '@' || (i > 0 && isWordChar(s[i - 1]))) return null
        var best: String? = null
        for (handle in handles) {
            if (handle.isEmpty() || (best != null && handle.length <= best.length)) continue
            val candidate = s.substring(i + 1, minOf(s.length, i + 1 + handle.length))
            if (candidate.length != handle.length || candidate.lowercase() != handle.lowercase()) continue
            val after = i + 1 + handle.length
            if (after < s.length && isWordChar(s[after])) continue
            best = candidate
        }
        return best
    }

    /** [mentions]: handles to highlight (a group's members), matched ignoring case, longest first. */
    fun parse(text: String, mentions: List<String> = emptyList(), selfHandle: String? = null): List<FormatNode> {
        val self = selfHandle?.lowercase()
        fun render(s: String, depth: Int): List<FormatNode> {
            val out = ArrayList<FormatNode>()
            fun push(t: String) {
                if (t.isEmpty()) return
                val last = out.lastOrNull()
                if (last is FormatNode.Plain) out[out.size - 1] = FormatNode.Plain(last.text + t) else out += FormatNode.Plain(t)
            }
            var pos = 0
            var i = 0
            while (i < s.length) {
                if (s[i] == '`') {
                    val close = s.indexOf('`', i + 1)
                    val newline = s.indexOf('\n', i + 1)
                    if (close > i + 1 && (newline < 0 || close < newline)) {
                        push(s.substring(pos, i))
                        out += FormatNode.Code(s.substring(i + 1, close))
                        i = close + 1
                        pos = i
                        continue
                    }
                }
                val end = linkEnd(s, i)
                if (end > i) {
                    push(s.substring(pos, i))
                    out += FormatNode.Link(s.substring(i, end))
                    i = end
                    pos = i
                    continue
                }
                if (s[i] == '@' && mentions.isNotEmpty()) {
                    val handle = mentionAt(s, i, mentions)
                    if (handle != null) {
                        push(s.substring(pos, i))
                        out += FormatNode.Mention("@$handle", self != null && handle.lowercase() == self)
                        i += 1 + handle.length
                        pos = i
                        continue
                    }
                }
                var matched = false
                if (depth < MAX_DEPTH) {
                    for ((token, style) in MARKERS) {
                        if (!s.startsWith(token, i) || !canOpenAfter(s, i)) continue
                        val inner = i + token.length
                        if (inner >= s.length || isSpace(s[inner]) || s[inner] == token[0]) continue
                        val close = findClose(s, i, token)
                        if (close < 0) continue
                        push(s.substring(pos, i))
                        val body = s.substring(inner, close)
                        // A spoiler's content is kept plain: nothing nested shows through while it's hidden.
                        out += FormatNode.Styled(style, if (style == FormatStyle.Spoiler) listOf(FormatNode.Plain(body)) else render(body, depth + 1))
                        i = close + token.length
                        pos = i
                        matched = true
                        break
                    }
                }
                if (!matched) i++
            }
            push(s.substring(pos))
            return out
        }
        return render(text, 0)
    }

    /** Whether the text has anything to render beyond plain characters. */
    fun hasFormatting(text: String, mentions: List<String> = emptyList(), selfHandle: String? = null): Boolean =
        parse(text, mentions, selfHandle).any { it !is FormatNode.Plain }

    /** The text a reader sees, markers removed and spoilers hidden (for previews and notifications). */
    fun toPlain(text: String): String {
        fun flatten(nodes: List<FormatNode>): String = nodes.joinToString("") { n ->
            when (n) {
                is FormatNode.Plain -> n.text
                is FormatNode.Code -> n.text
                is FormatNode.Mention -> n.text
                is FormatNode.Link -> n.url
                is FormatNode.Styled -> if (n.style == FormatStyle.Spoiler) "•••" else flatten(n.children)
            }
        }
        return flatten(parse(text))
    }

    /** Whether [text] mentions [handle] ("@handle", by the same rule as above). */
    fun mentions(text: String, handle: String): Boolean = parse(text, listOf(handle)).any { it is FormatNode.Mention }
}

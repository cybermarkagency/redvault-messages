// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.chats

import com.redvaultsystems.redvaultmessage.data.model.Message
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.data.model.ReactionView
import com.redvaultsystems.redvaultmessage.data.model.RichMessage
import com.redvaultsystems.redvaultmessage.devicekeys.ShownMessage
import java.time.LocalDate

/** One row of a thread's list, newest first (the list is drawn bottom-up, so index 0 is at the bottom). */
internal sealed interface ThreadRow {
    val key: String

    /** A message bubble, or a pin notice. */
    data class Item(val message: Message) : ThreadRow {
        override val key: String get() = message.id
    }

    /** The day of the messages under it, like the desktop's day headers. */
    data class Day(val date: LocalDate, val label: String) : ThreadRow {
        override val key: String get() = "day:$date"
    }
}

internal object ThreadRows {
    /** [newestFirst] with a day header above (after, in this order) the oldest message of each day. */
    fun build(newestFirst: List<Message>, dayOf: (Long) -> LocalDate, label: (LocalDate) -> String): List<ThreadRow> {
        val rows = ArrayList<ThreadRow>(newestFirst.size + 8)
        var day = newestFirst.firstOrNull()?.let { dayOf(it.createdAtMs) }
        for (i in newestFirst.indices) {
            rows += ThreadRow.Item(newestFirst[i])
            val current = day ?: break
            val older = newestFirst.getOrNull(i + 1)?.let { dayOf(it.createdAtMs) }
            if (older != current) rows += ThreadRow.Day(current, label(current))
            day = older
        }
        return rows
    }

    /** Where a message is in [rows], or -1 when it isn't loaded. */
    fun indexOf(rows: List<ThreadRow>, messageId: String): Int =
        rows.indexOfFirst { it is ThreadRow.Item && it.message.id.equals(messageId, ignoreCase = true) }
}

/** Search in one chat, like the desktop's: only what's loaded on this phone is searched, never the server. */
internal object ThreadSearch {
    /** Ids of the messages whose text contains [query] (any case), oldest first. Undecryptable ones never match. */
    fun hits(oldestFirst: List<Message>, query: String): List<String> {
        val needle = query.trim()
        if (needle.isEmpty()) return emptyList()
        return oldestFirst.filter { it.kind != MessageKind.Undecryptable && !it.deleted && it.text.contains(needle, ignoreCase = true) }.map { it.id }
    }

    /** [step] -1 for an older match, +1 for a newer one, wrapping around. -1 when there are none. */
    fun step(index: Int, count: Int, step: Int): Int = if (count <= 0) -1 else (((index + step) % count) + count) % count

    /** The matches changed (new or older messages): stay on [current] if it still matches, else the newest. */
    fun keep(hits: List<String>, current: String?): Int = hits.indexOf(current).takeIf { it >= 0 } ?: hits.lastIndex
}

/** Ticking several messages to forward or delete them together (the desktop's selection mode). */
internal object MessageSelection {
    /** A bubble that's on the server: not a pin notice, not still waiting in the Tor outbox. Device-key messages aren't there at all. */
    fun selectable(m: Message): Boolean = m.notice == null && !m.queued && !m.v3

    fun canForward(m: Message): Boolean =
        m.notice == null && m.poll == null && !m.queued && !m.v3 && (m.kind == MessageKind.Text || (m.kind == MessageKind.File && m.file != null))

    /** The ticked messages that can be forwarded, oldest first (the order they're sent in). */
    fun forwardable(messages: List<Message>, ids: Set<String>): List<Message> =
        messages.filter { it.id in ids && canForward(it) }.sortedBy { it.createdAtMs }

    /** Every ticked message is yours, so "Delete for everyone" applies to all of them. */
    fun allFromMe(messages: List<Message>, ids: Set<String>): Boolean =
        ids.isNotEmpty() && ids.all { id -> messages.firstOrNull { it.id == id }?.let { it.fromMe && it.kind != MessageKind.Undecryptable } == true }

    /** What goes with a message deleted for everyone: its edits and your own pin notices for it (both repeat its text). */
    fun deletedWith(message: Message, messages: List<Message>): List<String> =
        message.editMessageIds + messages.filter { it.fromMe && it.notice?.targetId == message.id.lowercase() }.map { it.id }

    /** Every id to delete for everyone: each of your own ticked messages and what goes with it, each once. */
    fun deletions(messages: List<Message>, ids: Set<String>): List<String> =
        messages.filter { it.id in ids && it.fromMe }.flatMap { listOf(it.id) + deletedWith(it, messages) }.distinct()
}

/** A device-key message as a chat bubble (toMessage in ui/src/state/V3Chat.tsx). */
internal object V3Messages {
    const val DELETED_TEXT = "This message was deleted."

    fun toMessage(shown: ShownMessage, selfUserId: String): Message {
        val r = shown.record
        val failed = r.status == "failed"
        val file = r.file
        fun quick(emoji: String) = RichMessage.REACTION_EMOJI.indexOf(emoji).let { if (it < 0) RichMessage.REACTION_EMOJI.size else it }
        return Message(
            id = r.id,
            fromMe = r.direction == "out",
            senderId = r.fromUserId,
            kind = if (file != null) MessageKind.File else MessageKind.Text,
            text = when {
                shown.deleted -> DELETED_TEXT
                file != null -> file.filename
                else -> r.text
            },
            file = file,
            fileStatus = if (file != null) "available" else null,
            reactions = shown.reactions.groupBy { it.emoji }
                .map { (emoji, people) -> ReactionView(emoji, people.size, people.any { it.userId == selfUserId }, null) }
                .sortedBy { quick(it.emoji) },
            editedAtMs = shown.editedAt,
            createdAtMs = r.ts,
            deleteAtMs = null,
            // No receipts on device keys yet: "Delivered" would claim more than we know.
            v3 = true,
            deleted = shown.deleted,
            // A send that failed after it was encrypted: the usual "Not sent" row with Retry and Discard.
            queued = failed,
            queueHeld = failed,
            queueError = if (failed) "Not sent: it couldn't reach the server." else null,
        )
    }
}

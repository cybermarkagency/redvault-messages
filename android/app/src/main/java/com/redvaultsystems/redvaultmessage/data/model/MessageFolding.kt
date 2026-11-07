package com.redvaultsystems.redvaultmessage.data.model

/** An edit message: [targetId] (lowercase) gets [text] as of [atMs]. */
class EditEvent(val id: String, val targetId: String, val senderId: String, val text: String, val atMs: Long)

/** A vote ([end] false; empty [options] takes it back) or a poll's end ([end] true). */
class PollEvent(val pollId: String, val senderId: String, val atMs: Long, val end: Boolean, val options: List<Int>)

/** Everyone who reacted to one message with one emoji; [firstAtMs] is when it was first used there. */
class ReactionTally(var firstAtMs: Long) {
    val senders = HashSet<String>()
    /** This user's reaction message, if they reacted with this emoji. */
    var myReactionId: String? = null
}

/**
 * Port of foldEditsAndVotes and pinnedFrom in ui/src/state/actions.ts (the
 * desktop app has the same rules): edits, votes and pins arrive as their
 * own messages and are folded into the messages they're about.
 */
object MessageFolding {
    const val MAX_PINS = 3

    /**
     * Applies each message's edits (only from its own sender, and only to
     * text) and each poll's votes. [senderOf] maps lowercase message ids to
     * their senders.
     */
    fun foldAll(
        messages: List<Message>,
        edits: List<EditEvent>,
        pollEvents: List<PollEvent>,
        senderOf: Map<String, String>,
        selfUserId: String,
    ): List<Message> {
        if (edits.isEmpty() && pollEvents.isEmpty()) return messages
        val editsByTarget = edits.groupBy { it.targetId }
        val eventsByPoll = pollEvents.groupBy { it.pollId }
        return messages.map { message ->
            val id = message.id.lowercase()
            val sender = senderOf[id]
            var out = message
            val mine = editsByTarget[id].orEmpty().filter { it.senderId == sender }.sortedBy { it.atMs }
            if (mine.isNotEmpty() && message.kind == MessageKind.Text && message.poll == null && message.notice == null) {
                val earlier = ArrayList<EditVersion>()
                var text = message.text
                var at = message.createdAtMs
                for (edit in mine) {
                    earlier += EditVersion(text, at)
                    text = edit.text
                    at = edit.atMs
                }
                out = out.copy(text = text, editedAtMs = at, earlierVersions = earlier, editMessageIds = mine.map { it.id })
            }
            message.poll?.let { poll -> out = out.copy(poll = tally(poll, eventsByPoll[id].orEmpty(), sender, selfUserId)) }
            out
        }
    }

    private fun tally(poll: PollView, events: List<PollEvent>, creator: String?, selfUserId: String): PollView {
        val sorted = events.sortedBy { it.atMs }
        val endedAt = sorted.firstOrNull { it.end && it.senderId == creator }?.atMs
        val latest = LinkedHashMap<String, List<Int>>()
        for (event in sorted) {
            if (event.end || (endedAt != null && event.atMs > endedAt)) continue
            var picked = event.options.filter { it < poll.options.size }
            if (!poll.multiple) picked = picked.take(1)
            latest[event.senderId] = picked
        }
        val counts = IntArray(poll.options.size)
        var voters = 0
        var mine = emptyList<Int>()
        for ((voter, picked) in latest) {
            if (picked.isEmpty()) continue
            voters++
            for (o in picked) counts[o]++
            if (voter == selfUserId) mine = picked
        }
        return poll.copy(closed = endedAt != null, counts = counts.toList(), mine = mine, voters = voters)
    }

    /** The quick six first, in their order; any other emoji after them, in the order they were first used. */
    fun orderReactions(byEmoji: Map<String, ReactionTally>): List<ReactionView> {
        fun quick(emoji: String) = RichMessage.REACTION_EMOJI.indexOf(emoji).let { if (it < 0) RichMessage.REACTION_EMOJI.size else it }
        return byEmoji.entries
            .sortedWith(compareBy({ quick(it.key) }, { it.value.firstAtMs }))
            .map { (emoji, tally) -> ReactionView(emoji, tally.senders.size, tally.myReactionId != null, tally.myReactionId) }
    }

    /** Pinned messages from the pin notices in a chat (oldest first); at most [MAX_PINS] stay pinned. */
    fun pinnedFrom(messages: List<Message>): List<PinnedView> {
        val pinned = ArrayList<PinnedView>()
        for (m in messages) {
            val notice = m.notice ?: continue
            pinned.removeAll { it.messageId == notice.targetId }
            if (notice.kind == NoticeKind.Unpin || notice.targetGone) continue
            pinned += PinnedView(notice.targetId, m.text, m.fromMe, m.senderId)
            if (pinned.size > MAX_PINS) pinned.removeAt(0)
        }
        return pinned
    }
}

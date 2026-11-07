package com.redvaultsystems.redvaultmessage.ui.channels

import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import com.redvaultsystems.redvaultmessage.data.model.ChannelPosts
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId

/** One row of a channel's thread, newest first (the list is drawn bottom-up, so index 0 is at the bottom). */
internal sealed interface ChannelRow {
    val key: String

    data class Post(val post: ChannelPostDto) : ChannelRow {
        override val key: String get() = post.id
    }

    /** The day of the posts under it. Keyed by the post it sits above, so a key can't repeat whatever order the server sends. */
    data class Day(val date: LocalDate, val label: String, val oldestPostId: String) : ChannelRow {
        override val key: String get() = "day:$oldestPostId"
    }
}

/** What sits under a channel's thread. */
internal enum class ChannelBottomBar { Composer, ReadOnly }

/** What can be done to one post, from a long press or its overflow button. */
internal enum class PostAction { CopyText, Forward, Delete }

/** What the owner's strip above the thread opens. */
internal enum class OwnerPanel { Scheduled, Suggestions }

internal data class OwnerStripItem(val panel: OwnerPanel, val label: String)

/** The channel screen's decisions, apart from the screen so they can be tested off a phone. */
internal object ChannelThread {
    /** The header's line. It is the honest disclosure: channels are readable by the server. */
    fun subtitle(isPrivate: Boolean): String =
        (if (isPrivate) "Private channel" else "Public channel") + " · not end-to-end encrypted"

    /** Only the owner gets a composer. An admin can take posts down but doesn't post, so reads like anyone else. */
    fun bottomBar(isOwner: Boolean): ChannelBottomBar = if (isOwner) ChannelBottomBar.Composer else ChannelBottomBar.ReadOnly

    /** The read-only bar's "Suggest a post". */
    fun canSuggest(isOwner: Boolean, acceptSuggestions: Boolean): Boolean = !isOwner && acceptSuggestions

    /** Only text can be forwarded into a chat (ChatSession.forwardChannelPost refuses anything else). */
    fun postActions(post: ChannelPostDto, isOwner: Boolean, isAdmin: Boolean): List<PostAction> = buildList {
        if (post.body.isNotBlank()) {
            add(PostAction.CopyText)
            add(PostAction.Forward)
        }
        if (isOwner || isAdmin) add(PostAction.Delete)
    }

    /**
     * The strip's entries, nothing when there's nothing to look at. [waitingSuggestions] is null when
     * the count couldn't be read: the entry stays, so the owner can still open the list.
     */
    fun ownerStrip(scheduledCount: Int, acceptSuggestions: Boolean, waitingSuggestions: Int?): List<OwnerStripItem> = buildList {
        if (scheduledCount > 0) add(OwnerStripItem(OwnerPanel.Scheduled, "Scheduled ($scheduledCount)"))
        if (acceptSuggestions && waitingSuggestions != 0) {
            add(OwnerStripItem(OwnerPanel.Suggestions, if (waitingSuggestions == null) "Suggested posts" else "Suggested posts ($waitingSuggestions)"))
        }
    }

    fun epochMs(createdAt: String): Long? = runCatching { Instant.parse(createdAt).toEpochMilli() }.getOrNull()

    /** The day a post appeared on, or null when its date can't be read. */
    fun dayOf(createdAt: String, zone: ZoneId): LocalDate? = epochMs(createdAt)?.let { Instant.ofEpochMilli(it).atZone(zone).toLocalDate() }

    /** [newestFirst] with a day row after (so drawn above) the oldest post of each day, as ThreadRows does for chats. A post with no readable date gets no row. */
    fun rows(newestFirst: List<ChannelPostDto>, dayOf: (ChannelPostDto) -> LocalDate?, label: (LocalDate) -> String): List<ChannelRow> {
        val rows = ArrayList<ChannelRow>(newestFirst.size + 8)
        newestFirst.forEachIndexed { i, post ->
            rows += ChannelRow.Post(post)
            val day = dayOf(post) ?: return@forEachIndexed
            val older = newestFirst.getOrNull(i + 1)?.let(dayOf)
            if (older != day) rows += ChannelRow.Day(day, label(day), post.id)
        }
        return rows
    }

    /** A page of older posts added to what's loaded, without repeats. */
    fun appendOlder(posts: List<ChannelPostDto>, page: List<ChannelPostDto>): List<ChannelPostDto> {
        val loaded = posts.mapTo(HashSet()) { it.id }
        return posts + page.filter { it.id !in loaded }
    }

    /** A post's footer: the time on its own when the date can't be read as one. */
    fun footer(post: ChannelPostDto, time: (Long) -> String): String =
        "${epochMs(post.createdAt)?.let(time) ?: post.createdAt} · ${ChannelPosts.views(post.views)}"

    /** The composer's post button: nothing half-uploaded or still recording, and something to post. */
    fun canPost(busy: Boolean, uploading: Boolean, recording: Boolean, hasText: Boolean, hasExtra: Boolean, hasMedia: Boolean, readyMedia: Int): Boolean =
        !busy && !uploading && !recording && (hasText || hasExtra || readyMedia > 0) && !(hasMedia && readyMedia == 0 && !hasText)

    /** An empty composer offers a voice clip, as a chat's does; anything to post makes it the send button. */
    fun showMic(hasText: Boolean, hasExtra: Boolean, hasMedia: Boolean): Boolean = !hasText && !hasExtra && !hasMedia
}

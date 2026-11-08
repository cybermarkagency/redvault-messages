// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.chats

import com.redvaultsystems.redvaultmessage.data.model.Conversation

/** The chat list's filter chips. The name is the label. */
internal enum class ChatFilter { All, Unread, Favorites, Groups, Verified }

/** The chat list's rules, kept apart from the screen so they can be tested. */
internal object ChatList {
    /**
     * Muted on this phone (Contact › Mute): a contact by their user id, a group by its id. Their calls
     * don't ring, their unread count is grey and they're left out of the Chats tab's badge.
     */
    fun isMuted(c: Conversation, muted: Set<String>): Boolean =
        c.id in muted || (!c.isGroup && c.meta.otherUserId.isNotEmpty() && c.meta.otherUserId in muted)

    /** A favorite contact (Contact › Favorite); groups can't be favorites. */
    fun isFavorite(c: Conversation, favorites: Set<String>): Boolean =
        !c.isGroup && c.meta.otherUserId.isNotEmpty() && c.meta.otherUserId in favorites

    fun matches(c: Conversation, filter: ChatFilter, query: String, favorites: Set<String>): Boolean =
        (query.isBlank() || c.handle.contains(query.trim(), ignoreCase = true)) &&
            when (filter) {
                ChatFilter.All -> true
                ChatFilter.Unread -> c.unread > 0
                ChatFilter.Favorites -> isFavorite(c, favorites)
                ChatFilter.Groups -> c.isGroup
                ChatFilter.Verified -> !c.isGroup && c.verified
            }

    /** The Chats tab's badge: chats with something unread, muted ones left out (as on the desktop). */
    fun unreadForBadge(conversations: List<Conversation>, muted: Set<String>): Int =
        conversations.count { it.unread > 0 && !isMuted(it, muted) }

    /**
     * A tapped "new message" notification. The push names no chat (so neither Google nor anyone watching
     * learns which chat is active): the phone opens the one chat with unread messages, if there's exactly
     * one, and otherwise stays on the chat list.
     */
    fun chatToOpen(conversations: List<Conversation>): String? =
        conversations.filter { it.unread > 0 }.singleOrNull()?.id
}

/** What a share from another app may hand over. */
internal object SharedContent {
    /** Only content:// from other apps: never a file path, and never one of this app's own providers (its decrypted files). */
    fun acceptable(scheme: String?, authority: String?, ownPackage: String): Boolean {
        if (!scheme.equals("content", ignoreCase = true)) return false
        val host = authority?.substringAfterLast('@')?.lowercase()?.takeIf { it.isNotEmpty() } ?: return false
        val own = ownPackage.lowercase()
        return host != own && !host.startsWith("$own.")
    }

    /** At most this many files from one share. */
    const val MAX_FILES = 20

    /** Shared text goes into the message box; this keeps a huge one from filling it. */
    const val MAX_TEXT = 8_000
}

package com.redvaultsystems.redvaultmessage.ui.chats

import com.redvaultsystems.redvaultmessage.ui.chats.ChatTestData.chat
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ChatListLogicTest {
    private val alice = chat("c-alice", otherUserId = "u-alice", handle = "alice", unread = 2, verified = true)
    private val bob = chat("c-bob", otherUserId = "u-bob", handle = "bob")
    private val team = chat("g-team", isGroup = true, handle = "Team", unread = 1)

    @Test
    fun mutedIsAContactsUserIdOrAGroupsId() {
        assertTrue(ChatList.isMuted(alice, setOf("u-alice")))
        assertTrue(ChatList.isMuted(team, setOf("g-team")))
        assertFalse(ChatList.isMuted(bob, setOf("u-alice", "g-team")))
    }

    @Test
    fun onlyContactsCanBeFavorites() {
        assertTrue(ChatList.isFavorite(alice, setOf("u-alice")))
        assertFalse(ChatList.isFavorite(team, setOf("g-team")))
    }

    @Test
    fun filterChips() {
        val all = listOf(alice, bob, team)
        fun shown(filter: ChatFilter, query: String = "", favorites: Set<String> = emptySet()) =
            all.filter { ChatList.matches(it, filter, query, favorites) }.map { it.id }
        assertEquals(listOf("c-alice", "c-bob", "g-team"), shown(ChatFilter.All))
        assertEquals(listOf("c-alice", "g-team"), shown(ChatFilter.Unread))
        assertEquals(listOf("c-bob"), shown(ChatFilter.Favorites, favorites = setOf("u-bob", "g-team")))
        assertEquals(listOf("g-team"), shown(ChatFilter.Groups))
        assertEquals(listOf("c-alice"), shown(ChatFilter.Verified))
        assertEquals(listOf("c-bob"), shown(ChatFilter.All, query = " BO "))
        // The order matches the desktop's chips, with Verified after them.
        assertEquals(listOf("All", "Unread", "Favorites", "Groups", "Verified"), ChatFilter.entries.map { it.name })
    }

    @Test
    fun mutedChatsStayOutOfTheTabBadge() {
        assertEquals(2, ChatList.unreadForBadge(listOf(alice, bob, team), emptySet()))
        assertEquals(1, ChatList.unreadForBadge(listOf(alice, bob, team), setOf("u-alice")))
    }

    @Test
    fun aMessageNotificationOpensTheOnlyChatWithUnreadMessages() {
        assertEquals("c-alice", ChatList.chatToOpen(listOf(alice, bob)))
        // Several, or none: the chat list.
        assertNull(ChatList.chatToOpen(listOf(alice, bob, team)))
        assertNull(ChatList.chatToOpen(listOf(bob)))
    }

    @Test
    fun sharesTakeOnlyOtherAppsContent() {
        val own = "com.redvaultsystems.redvaultmessage"
        assertTrue(SharedContent.acceptable("content", "com.google.android.apps.photos.contentprovider", own))
        assertTrue(SharedContent.acceptable("CONTENT", "media", own))
        // Never a file path, and never this app's own decrypted files, whoever asks.
        assertFalse(SharedContent.acceptable("file", "", own))
        assertFalse(SharedContent.acceptable("content", "$own.files", own))
        assertFalse(SharedContent.acceptable("content", own, own))
        assertFalse(SharedContent.acceptable("content", "0@$own.files", own))
        assertFalse(SharedContent.acceptable("content", null, own))
        assertFalse(SharedContent.acceptable(null, "media", own))
        assertFalse(SharedContent.acceptable("https", "example.com", own))
        // A look-alike that only starts with the same name isn't ours.
        assertTrue(SharedContent.acceptable("content", "${own}x.provider", own))
    }
}

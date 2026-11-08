// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

/** Mirrors the web's ui/src/test/channelsApi.test.ts for lib/channels.ts. */
class ChannelsTest {
    private val id = "11111111-2222-4333-8444-555555555555"
    private val site = "https://www.redvaultsystems.com/redvaultmessages/"
    private val channel = ChannelDto(id = id, name = "News", lastPostAt = "2026-10-18T09:00:00.000Z")

    @Test
    fun followingCountsWhatsAlreadyPostedAsSeenAndOnlyNewerPostsAsNew() {
        val list = Channels.follow(emptyList(), channel)
        assertEquals(listOf(FollowedChannel(id, "News", channel.lastPostAt)), list)
        assertSame(list, Channels.follow(list, channel))
        assertFalse(Channels.hasUnread(list[0], channel))
        val newer = channel.copy(name = "News+", lastPostAt = "2026-10-18T10:00:00.000Z")
        assertTrue(Channels.hasUnread(list[0], newer))
        assertEquals(listOf(FollowedChannel(id, "News+", newer.lastPostAt)), Channels.markSeen(list, newer))
        assertEquals(emptyList<FollowedChannel>(), Channels.unfollow(list, id))
        assertTrue(Channels.isFollowing(list, id))
        assertFalse(Channels.isFollowing(list, "other"))
    }

    @Test
    fun theUnreadDotFollowsTheWebsRules() {
        val seen = FollowedChannel(id, "News", "2026-10-18T09:00:00.000Z")
        // Nothing posted: nothing new.
        assertFalse(Channels.hasUnread(seen, channel.copy(lastPostAt = null)))
        // Followed before anything was posted, and now there's a post.
        assertTrue(Channels.hasUnread(seen.copy(seenAt = null), channel))
        // Older isn't new; a timestamp that doesn't parse never counts as newer.
        assertFalse(Channels.hasUnread(seen, channel.copy(lastPostAt = "2026-10-18T08:59:59.999Z")))
        assertFalse(Channels.hasUnread(seen, channel.copy(lastPostAt = "yesterday")))
        assertFalse(Channels.hasUnread(seen.copy(seenAt = "garbage"), channel))
    }

    @Test
    fun aPublicChannelsLinkIsPlainAndAPrivateOneCarriesItsSecretInTheFragment() {
        assertEquals("$site?channel=$id", Channels.url(site, id))
        assertEquals("$site?channel=$id", Channels.url(site.trimEnd('/'), id))
        assertFalse(Channels.url(site, id).contains('#'))
        assertEquals("$site?channel=$id#$secret", Channels.url(site, id, secret))
        assertEquals("$site?channel=$id", Channels.url(site, id, ""))
    }

    @Test
    fun onlyThisAppsOwnChannelLinksOpenInTheApp() {
        assertEquals(ChannelLink(id, null), Channels.parseOwn("$site?channel=$id", site))
        assertEquals(ChannelLink(id, null), Channels.parseOwn("  $site?x=1&channel=$id#top ", site))
        assertEquals(ChannelLink(id, secret), Channels.parseOwn("$site?channel=$id#$secret", site))
        // Ids come back lowercase, like the desktop app's parseChannelLink.
        assertEquals(ChannelLink(id, null), Channels.parseOwn("$site?channel=${id.uppercase()}", site))
        assertNull(Channels.parseOwn("https://evil.example/redvaultmessages/?channel=$id", site))
        assertNull(Channels.parseOwn("$site?channel=not-a-channel", site))
        assertNull(Channels.parseOwn("$site?invite=$id", site))
        assertNull(Channels.parseOwn(site, site))
    }

    @Test
    fun aPastedLinkIsFoundInsideAMessage() {
        assertEquals(ChannelLink(id, secret), Channels.parse("Join us: $site?channel=$id#$secret and say hi"))
        assertEquals(ChannelLink(id, null), Channels.parse("$site?channel=$id"))
        // A fragment that isn't a secret is ignored, as the web ignores it.
        assertEquals(ChannelLink(id, null), Channels.parse("$site?channel=$id#top"))
        assertEquals(ChannelLink(id, null), Channels.parse("$site?channel=$id#${secret.drop(1)}"))
        // channel= has to start a query parameter, and the id has to end there.
        assertNull(Channels.parse("$site?xchannel=$id"))
        assertNull(Channels.parse("channel=$id"))
        assertNull(Channels.parse("$site?channel=${id}0"))
        assertNull(Channels.parse("$site?channel=${id.dropLast(1)}"))
        assertEquals(ChannelLink(id, null), Channels.parse("$site?channel=nope&channel=$id"))
        assertNull(Channels.parse("nothing here"))
    }

    @Test
    fun followingAPrivateChannelKeepsItsSecretAndMutingKeepsItsPlace() {
        val private = channel.copy(isPrivate = true)
        val list = Channels.follow(emptyList(), private, secret)
        assertEquals(listOf(FollowedChannel(id, "News", channel.lastPostAt, secret)), list)
        // A newer secret (the link was reset) replaces it; none keeps the one there is.
        val reset = "B".repeat(42) + "A"
        assertEquals(reset, Channels.markSeen(list, private, reset)[0].secret)
        assertEquals(secret, Channels.markSeen(list, private)[0].secret)
        assertNull(Channels.follow(emptyList(), channel, "")[0].secret)
        val muted = Channels.setMuted(list, id, true)
        assertTrue(muted[0].muted)
        assertFalse(Channels.setMuted(muted, id, false)[0].muted)
    }

    @Test
    fun theFollowListReadsTheWebsShape() {
        val json = Json { ignoreUnknownKeys = true }
        val web = """[{"id":"$id","name":"News","seenAt":null},{"id":"$id","name":"Secret","seenAt":"2026-10-18T09:00:00.000Z","secret":"$secret","muted":true}]"""
        val list = json.decodeFromString(ListSerializer(FollowedChannel.serializer()), web)
        assertEquals(FollowedChannel(id, "News", null), list[0])
        assertEquals(FollowedChannel(id, "Secret", "2026-10-18T09:00:00.000Z", secret, muted = true), list[1])
    }

    private companion object {
        /** 32 bytes of 0x01..0x20 as base64url, a valid secret (desktop/tests/vectors/channel_vectors.json). */
        const val secret = "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA"
    }
}

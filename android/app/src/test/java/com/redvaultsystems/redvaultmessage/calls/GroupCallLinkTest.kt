// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.calls

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** Mirrors the web's groupCallLink behaviour (ui/src/lib/groupCallLink.ts and its use in CallsScreen's JoinWithLinkDialog). */
class GroupCallLinkTest {
    private val id = "6f1d2c3e-4b5a-4c6d-8e7f-0a1b2c3d4e5f"
    private val site = "https://www.redvaultsystems.com/redvaultmessages/"

    @Test
    fun theLinkIsTheWebAppsAddressWithTheCallId() {
        assertEquals("$site?group-call=$id", GroupCallLink.url(site, id))
        // A base without its trailing slash gets one.
        assertEquals("$site?group-call=$id", GroupCallLink.url(site.trimEnd('/'), id))
    }

    @Test
    fun theInviteMessageIsWordForWordTheWebs() {
        val url = GroupCallLink.url(site, id)
        assertEquals(
            "📞 Group video call started. To join, open Calls, tap \"Join with a link\" and paste this link:\n$url",
            GroupCallLink.inviteText("video", url),
        )
        assertEquals(
            "📞 Group voice call started. To join, open Calls, tap \"Join with a link\" and paste this link:\n$url",
            GroupCallLink.inviteText("voice", url),
        )
    }

    @Test
    fun parsesABareLinkAndTheWholeMessageItCameIn() {
        val url = GroupCallLink.url(site, id)
        assertEquals(id, GroupCallLink.parse(url))
        assertEquals(id, GroupCallLink.parse("  $url \n"))
        assertEquals(id, GroupCallLink.parse(GroupCallLink.inviteText("video", url)))
        // Whatever app address it points at: the server decides if you're allowed in.
        assertEquals(id, GroupCallLink.parse("http://10.0.2.2:3077/redvaultmessages/?group-call=$id"))
        assertEquals(id, GroupCallLink.parse("/?group-call=$id"))
    }

    @Test
    fun theIdIsLowerCasedAndMustBeAUuid() {
        assertEquals(id, GroupCallLink.parse("$site?group-call=${id.uppercase()}"))
        assertNull(GroupCallLink.parse("$site?group-call=not-a-uuid"))
        assertNull(GroupCallLink.parse("$site?group-call="))
        assertNull(GroupCallLink.parse("$site?group-call=${id}0"))
        assertNull(GroupCallLink.parse("$site?other=$id"))
        assertNull(GroupCallLink.parse("hello"))
        assertNull(GroupCallLink.parse(""))
        // A bare id isn't a link.
        assertNull(GroupCallLink.parse(id))
    }

    @Test
    fun otherCallLinksAreNotGroupLinks() {
        // One-to-one call links (?join=<id>#<secret>) are the other box's job.
        assertNull(GroupCallLink.parse("$site?join=abcdefghijkl#c2VjcmV0"))
    }

    @Test
    fun aQueryParameterAfterOthersOrBeforeAFragmentIsFound() {
        assertEquals(id, GroupCallLink.parse("$site?x=1&group-call=$id&y=2"))
        assertEquals(id, GroupCallLink.parse("$site?group-call=$id#top"))
        assertNull(GroupCallLink.parse("$site#?group-call=$id"))
    }

    @Test
    fun onlyLinksToThisAppsOwnAddressJoinInTheApp() {
        assertEquals(id, GroupCallLink.parseOwn("$site?group-call=$id", site))
        assertEquals(id, GroupCallLink.parseOwn("$site?group-call=$id", site.trimEnd('/')))
        assertEquals(id, GroupCallLink.parseOwn("HTTPS://WWW.REDVAULTSYSTEMS.COM/redvaultmessages?group-call=$id", site))
        // Someone else's page that merely carries the parameter is an ordinary link.
        assertNull(GroupCallLink.parseOwn("https://evil.example/redvaultmessages/?group-call=$id", site))
        assertNull(GroupCallLink.parseOwn("https://www.redvaultsystems.com/other/?group-call=$id", site))
        assertNull(GroupCallLink.parseOwn("$site?other=$id", site))
    }
}

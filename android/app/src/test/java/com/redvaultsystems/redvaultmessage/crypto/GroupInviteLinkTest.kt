// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Invite link addresses, against what the web's own parseGroupInvite, inviteSecretFromFragment and
 * groupInviteUrl (ui/src/lib/groupInviteCrypto.ts) returned for the same inputs.
 */
class GroupInviteLinkTest {
    private val site = "https://www.redvaultsystems.com/redvaultmessages/"
    private val s = "AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dw"
    private val secretBytes = Base64Codec.decode("AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dw=")

    @Test
    fun theLinkIsTheWebAppsAddressWithTheIdAndTheSecretInTheFragment() {
        // groupInviteUrl("L1", secret) with the app served at http://localhost:3000/redvaultmessages/.
        assertEquals("http://localhost:3000/redvaultmessages/?invite=L1#$s", GroupInviteLink.url("http://localhost:3000/redvaultmessages/", "L1", secretBytes))
        assertEquals("$site?invite=L1#$s", GroupInviteLink.url(site.trimEnd('/'), "L1", secretBytes))
        assertEquals(s, GroupInviteLink.encodeSecret(secretBytes))
    }

    @Test
    fun readsTheSecretOnlyWhereTheWebDoes() {
        // [fragment, what inviteSecretFromFragment gave (standard base64), or null].
        val cases = listOf(
            s to "AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dw=",
            "#$s" to "AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dw=",
            "$s=" to null,
            s.dropLast(1) to null,
            "${s}A" to null,
            // The last character's unused bits set: libsodium refuses it.
            "AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dx" to null,
            "" to null,
            "not base64!" to null,
        )
        for ((fragment, expected) in cases) {
            val got = GroupInviteLink.secretFromFragment(fragment)
            if (expected == null) assertNull(fragment, got) else assertArrayEquals(fragment, Base64Codec.decode(expected), got)
        }
        // Standard base64's own characters aren't base64url.
        assertNull(GroupInviteLink.secretFromFragment("+" + s.drop(1)))
        assertNull(GroupInviteLink.secretFromFragment("/" + s.drop(1)))
    }

    @Test
    fun parsesPastedLinksExactlyLikeTheWeb() {
        val ok = GroupInviteLink.Parsed("L1", s)
        // [what was pasted, what parseGroupInvite returned].
        val cases = listOf(
            "$site?invite=L1#$s" to ok,
            "Join us: $site?invite=L1#$s see you" to ok,
            "  $site?invite=L1#$s\n" to ok,
            "$site?invite=L1" to null,
            "/?invite=L1#$s" to ok,
            "?invite=L1#$s" to ok,
            "http://10.0.2.2:3077/redvaultmessages/?invite=Ab_-9#$s" to GroupInviteLink.Parsed("Ab_-9", s),
            "$site?x=1&invite=L1&y=2#$s" to ok,
            "$site?invite=%4C1#$s" to ok,
            "$site?invite=L+1#$s" to null,
            "$site?invite=L.1#$s" to null,
            "$site?invite=${"a".repeat(64)}#$s" to GroupInviteLink.Parsed("a".repeat(64), s),
            "$site?invite=${"a".repeat(65)}#$s" to null,
            "$site?invite=#$s" to null,
            "$site?invite=L1#${s.dropLast(1)}" to null,
            "$site?invite=L1#$s=" to null,
            "$site?invite=L1#$s#more" to null,
            "$site#?invite=L1#$s" to null,
            "$site?join=L1#$s" to null,
            "$site?group-call=6f1d2c3e-4b5a-4c6d-8e7f-0a1b2c3d4e5f" to null,
            "hello" to null,
            "" to null,
        )
        for ((pasted, expected) in cases) assertEquals(pasted, expected, GroupInviteLink.parse(pasted))
    }

    @Test
    fun onlyLinksToThisAppsOwnAddressOpenInTheApp() {
        val ok = GroupInviteLink.Parsed("L1", s)
        assertEquals(ok, GroupInviteLink.parseOwn("$site?invite=L1#$s", site))
        assertEquals(ok, GroupInviteLink.parseOwn("$site?invite=L1#$s", site.trimEnd('/')))
        assertEquals(ok, GroupInviteLink.parseOwn("HTTPS://WWW.REDVAULTSYSTEMS.COM/redvaultmessages?invite=L1#$s", site))
        // Someone else's page that merely carries the parameter is an ordinary link.
        assertNull(GroupInviteLink.parseOwn("https://evil.example/redvaultmessages/?invite=L1#$s", site))
        assertNull(GroupInviteLink.parseOwn("$site?invite=L1", site))
    }
}

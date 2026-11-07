package com.redvaultsystems.redvaultmessage.data.api

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Parses response bodies in the exact shapes the live server returns, so a
 * mismatch fails here instead of as "Something went wrong." in the app.
 */
class DtoParsingTest {
    private val json = ApiClient.json

    @Test
    fun planStatusAsTheServerSendsIt() {
        val cancelling = json.decodeFromString<PlanStatusDto>(
            """{"tier":"pro","source":"stripe","renews":false,"endsOn":"2026-10-26","canCancel":false,"canResume":true}""",
        )
        assertEquals("2026-10-26", cancelling.endsOn)
        assertTrue(cancelling.canResume)
        val free = json.decodeFromString<PlanStatusDto>(
            """{"tier":"free","source":null,"renews":false,"endsOn":null,"canCancel":false,"canResume":false}""",
        )
        assertNull(free.source)
        assertNull(free.endsOn)
    }

    @Test
    fun receiptsHaveNoTimestamp() {
        val receipts = json.decodeFromString<List<ReceiptDto>>(
            """[{"messageId":"m1","userId":"u1","status":"read"}]""",
        )
        assertEquals("read", receipts.single().status)
    }

    @Test
    fun listedMessageHasNoDeleteAtOrVerification() {
        val message = json.decodeFromString<MessageDto>(
            """{"id":"m1","conversationId":"c1","senderId":"u1","ciphertext":"AA==","nonce":"AA==",
               "createdAt":"2026-09-24T09:24:19.676Z","editedAt":null,"deletedAt":null,
               "attachments":[{"objectId":"o1","status":"available","encryptedSize":17}],"verification":null}""",
        )
        assertNull(message.deleteAt)
        assertNull(message.verification)
        assertEquals("o1", message.attachments.single().objectId)
    }

    @Test
    fun conversationParticipantAndUser() {
        val conversation = json.decodeFromString<ConversationDto>(
            """{"id":"c1","isGroup":false,"role":"owner","nameCiphertext":null,"nameNonce":null,"wrappedKey":"AA==",
               "disappearingSeconds":null,"createdBy":"u1","createdAt":"2026-09-24T09:24:19.120Z"}""",
        )
        assertNull(conversation.disappearingSeconds)
        json.decodeFromString<ParticipantDto>("""{"userId":"u1","role":"owner","joinedAt":"2026-09-24T09:24:19.120Z"}""")
        val user = json.decodeFromString<UserResult>(
            """{"userId":"u1","username":"a","publicKey":"AA==","avatarUrl":null,"createdAt":"2026-09-24T09:24:19.120Z","deleted":false}""",
        )
        assertTrue(!user.deleted)
    }

    @Test
    fun friendRequestLookupAndMe() {
        val request = json.decodeFromString<FriendRequestDto>(
            """{"id":"r1","requesterId":"u1","recipientId":"u2","status":"pending","createdAt":"2026-09-24T09:24:19.120Z","respondedAt":null}""",
        )
        assertEquals("pending", request.status)
        val found = json.decodeFromString<UserLookupResult>("""{"userId":"u2","publicKey":"AA==","avatarUrl":null}""")
        assertEquals("u2", found.userId)
        val me = json.decodeFromString<MeResult>(
            """{"userId":"u1","username":"a","publicKey":"AA==","acceptMessageRequests":true,"transparencyMode":false,
               "avatarUrl":"/api/users/u1/avatar?v=1","role":"user","subscriptionTier":"free","mfaEnabled":false}""",
        )
        assertTrue(me.acceptMessageRequests)
    }

    @Test
    fun settingsRequestSendsOnlyTheChangedField() {
        assertEquals("""{"transparencyMode":true}""", json.encodeToString(SettingsRequest.serializer(), SettingsRequest(transparencyMode = true)))
    }
}

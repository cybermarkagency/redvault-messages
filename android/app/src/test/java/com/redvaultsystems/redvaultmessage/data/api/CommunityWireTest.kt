package com.redvaultsystems.redvaultmessage.data.api

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import retrofit2.http.GET
import retrofit2.http.POST

/**
 * Communities on the wire, in the shapes server/src/routes/communityRoutes.ts and conversationRoutes.ts
 * send, and proof that this app's API surface has no way to buy anything.
 */
class CommunityWireTest {
    private val json = ApiClient.json

    @Test
    fun theApiHasNoCheckoutOrPaymentPortal() {
        val paths = CommunityApi::class.java.declaredMethods.flatMap { method ->
            listOfNotNull(method.getAnnotation(GET::class.java)?.value, method.getAnnotation(POST::class.java)?.value)
        }
        assertTrue("the interface must actually be scanned", paths.size >= 15)
        for (path in paths) {
            assertFalse("$path must not exist on Android (Google Play payments policy)", "checkout" in path || "portal" in path)
        }
    }

    @Test
    fun aCommunityConversationCarriesItsCommunityAndEpoch() {
        val conversation = json.decodeFromString<ConversationDto>(
            """{"id":"c1","isGroup":true,"role":"member","nameCiphertext":null,"nameNonce":null,"wrappedKey":"AA==",
               "disappearingSeconds":null,"createdBy":"u1","createdAt":"2026-10-18T12:00:00.000Z",
               "community":{"id":"3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f","name":"Night Market Notes","kind":"channel","status":"published",
               "hasAvatar":false,"role":"member","canPost":false,"keyEpoch":2,"rekeyRequired":true}}""",
        )
        val community = conversation.community!!
        assertEquals(2L, community.keyEpoch)
        assertTrue(community.rekeyRequired)
        assertFalse(community.canPost)
        // An ordinary group has no block.
        assertNull(json.decodeFromString<ConversationDto>("""{"id":"c2","isGroup":true,"role":"owner","wrappedKey":"AA==","createdBy":"u1","createdAt":"2026-10-18T12:00:00.000Z"}""").community)
    }

    @Test
    fun messagesSendTheirEpochOnlyInACommunity() {
        assertEquals("""{"ciphertext":"AA==","nonce":"BB=="}""", json.encodeToString(SendMessageRequest.serializer(), SendMessageRequest("AA==", "BB==")))
        assertEquals(
            """{"ciphertext":"AA==","nonce":"BB==","keyEpoch":3}""",
            json.encodeToString(SendMessageRequest.serializer(), SendMessageRequest("AA==", "BB==", keyEpoch = 3)),
        )
        val listed = json.decodeFromString<MessageDto>(
            """{"id":"m1","conversationId":"c1","senderId":"u1","ciphertext":"AA==","nonce":"AA==","protocol":1,"senderDeviceId":null,
               "keyEpoch":3,"createdAt":"2026-10-18T12:00:00.000Z","editedAt":null,"deletedAt":null,"attachments":[]}""",
        )
        assertEquals(3, listed.keyEpoch)
        // Outside a community the epoch is the group key epoch (0 until a group is re-keyed), never missing.
        assertEquals(0, json.decodeFromString<MessageDto>("""{"id":"m2","conversationId":"c1","senderId":"u1","createdAt":"2026-10-18T12:00:00.000Z"}""").keyEpoch)
    }

    @Test
    fun keyRequestsAreWhatTheServersStrictSchemasAccept() {
        // enrollSchema and rotateSchema are .strict(): no extra fields.
        assertEquals(
            """{"userId":"u2","epoch":1,"wrappedKey":"AA=="}""",
            json.encodeToString(EnrollRequest.serializer(), EnrollRequest("u2", 1, "AA==")),
        )
        assertEquals(
            """{"fromEpoch":1,"keys":[{"userId":"u1","wrappedKey":"AA=="}]}""",
            json.encodeToString(RotateRequest.serializer(), RotateRequest(1, listOf(MemberKeyDto("u1", "AA==")))),
        )
        // A report without disclosure sends no evidence at all.
        assertEquals(
            """{"targetType":"content","targetUserId":"u3","category":"spam","details":""}""",
            json.encodeToString(CommunityReportRequest.serializer(), CommunityReportRequest("content", "u3", "spam", "")),
        )
        assertEquals(
            """{"targetType":"content","category":"spam","details":"","evidenceMessageId":"m1","evidencePlaintext":"hi","disclosureAcknowledged":true}""",
            json.encodeToString(
                CommunityReportRequest.serializer(),
                CommunityReportRequest("content", null, "spam", "", evidenceMessageId = "m1", evidencePlaintext = "hi", disclosureAcknowledged = true),
            ),
        )
    }

    @Test
    fun keyStateRosterAndTasks() {
        val keys = json.decodeFromString<CommunityKeysDto>(
            """{"conversationId":"c1","keyEpoch":1,"rekeyRequired":false,"keys":[{"epoch":0,"wrappedKey":"AA=="},{"epoch":1,"wrappedKey":"BB=="}]}""",
        )
        assertEquals(listOf(0L, 1L), keys.keys.map { it.epoch })
        val roster = json.decodeFromString<RosterDto>("""{"keyEpoch":1,"rekeyRequired":true,"members":[{"userId":"u1","publicKey":"AA=="}]}""")
        assertEquals("u1", roster.members.single().userId)
        val task = json.decodeFromString<List<KeyTaskDto>>(
            """[{"communityId":"x","conversationId":"c1","keyEpoch":1,"rekeyRequired":false,"pendingEnrollments":2,"canPost":true}]""",
        ).single()
        assertEquals(2, task.pendingEnrollments)
    }
}

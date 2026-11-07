package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pins community epoch keys to the web byte for byte. Every vector below came out of the web's own
 * ui/src/lib/communityCrypto.ts (libsodium-wrappers-sumo, run under vitest): a member's keypair, the bound
 * payload, sealed copies of epochs 0, 1 and 2^32-1, a message under epoch 1, a rotation and an enrollment.
 * The other direction (copies this app seals, opened by the web) is ui/src/test/communityCryptoInterop.test.ts.
 * If these fail, a community key made on the web won't open in this app, or the other way round. Runs on the
 * JVM with lazysodium-java: the same libsodium behind the same LazySodium API the app uses on a phone.
 */
class CommunityCryptoInteropTest {
    private val sodium = LazySodiumJava(SodiumJava())
    private val e2e = E2eCrypto(sodium)
    private val crypto = CommunityCrypto(e2e)

    private fun b64(s: String) = Base64Codec.decode(s)
    private fun hex(bytes: ByteArray) = bytes.joinToString("") { "%02x".format(it) }

    private val community = "3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f"
    private val other = "9e8d7c6b-5a49-4382-a1b0-c9d8e7f6a5b4"
    private val member = KeyPair(b64("V1ddK9z/rDvMog1//WRAXiTuJGbBR1rE413hikM3nWo="), b64("TLrnxvQ11QLUzbzVWkFPXNGpxkjqyQf/6liRtSxHWmw="))
    private val waiting = KeyPair(b64("vP05/vaY2CqWehsKzG8ie3t44uBu2W7qdzeKzN1xZFc="), b64("vdjL3KlaCxWetaRwqQVyO/TxKAvlcoBOzQwclS9WasA="))

    // Bytes (i * 3 + 1), (200 - i * 5) and (i xor 0x5a).
    private val k0 = b64("AQQHCg0QExYZHB8iJSgrLjE0Nzo9QENGSUxPUlVYW14=")
    private val k1 = b64("yMO+ubSvqqWgm5aRjIeCfXhzbmlkX1pVUEtGQTw3Mi0=")
    private val kMax = b64("WltYWV5fXF1SU1BRVldUVUpLSElOT0xNQkNAQUZHREU=")

    // sealEpochKey(k, epoch, community, member.publicKey)
    private val sealed0 = "VijGb08GqTTK2XnKhKJKuwJl5ANi4aZbk6Tc6cdRwwMk24EfAZ0LqDcFox9UsrD4T52972vujI+TrzR3hc7/Iz0i3PRYlfLiB9WaHIcVN0OcEK/fijNBRdvKdTtziYZ89kVWkriCDMAI"
    private val sealed1 = "V32RQyRjAGGbHDpePBwO2aH3htsm6pfHBomZeUka8kO3sQz9CrgL0+vlZ+2W/VPgg0gzaxYDekop0uvqSughtgLzoc9plCO7uHE2rVUm24XEJ45B41UQiQ8JgOcB8zST7hiXGjuRRdiN"
    private val sealedMax = "tjS/ZhON7RZ2vR+5SlVIBZWAHS+zxW5gWFUuuxOuqTIiwAFaEnhJRH+uWEOygEzw7h10pXa8kRab8mEboV71d4QUlqFMbmBSnsQpWogFN7/I+SJBmbkLJrDMNr61DFSOidSk94xPuleV"
    // k0 sealed again, bound to epoch 1: what a malicious rotator reusing a key would send.
    private val resealedK0AsEpoch1 = "jOTTlj96Ziry80nmm5f60F8F+y61bwcxEPVFsEJmUU2u5Q7skg5g2LU16AtxZzito2VmpjK4b6hUsCYhc2eB1JC+pjGPHlNCVCfDkc/fQnvfm3Z9nMWH+puQJ2SXdjNoc9nWVNKAIXL5"
    // k0 bound to the other community.
    private val foreign0 = "sYP1q8/HFpovgv4gFF4LY2yxZOVG5xZVvEVr+fHan3mbmbuTwYmhGJGalZ2LOUH4VANk9pPHTVzAo8K7gOuOzrPraerv6k6pekEDmY6Q5E4PeyzM4sheOwSQvk/WP0Izp09YQmIEjavD"
    // crypto.sealConversationKey(k0, member.publicKey): an ordinary group key, with no binding.
    private val plainGroupKey = "AQbtMpz5HFfAJemplVgflQ/bGGIn1rqv+nZq6whv7iHHmEjfEY/behaYZI87vlSkYXffKs2V8rotWEoZbDOBq4D5FS1yZTQO1MK7gKs5hbs="

    // encryptForCommunity(ring at epoch 1, text)
    private val messageText = "Night market 🌙 \"quoted\" \\ ünïcode\nline two"
    private val messageCiphertext = "1PHzIhJ60Oh2a2/QRYc0zxLzbq5LqrhbxiDo8Gyw2dAMauojphB+Q6RihUmpXSDzd7AujIdF4B8NjPAGUSPi"
    private val messageNonce = "43PAqIeVoOb+NUj+9qXpVpWoOTCmeH8S"

    // prepareRotation(ring at epoch 1, roster [member]) and prepareEnrollments(ring, [waiting])
    private val rotationKey = b64("qFWdWudV1scBIkBtYlG41DDMx132mck4HdblicPbs3s=")
    private val rotationWrapped = "59Px7Us6LYz1/6jtb+bO0AyApGVyL2x9DHv9yln+mnw5AXXE5e90ikTrz9za6nTEtsAQlIhscidOaomEeJZdq2pjYE36DbamT6xR0tQx+5U/m1azQ2rGhxVpnVLYnOXqDE7zp9dclpph"
    private val enrollmentWrapped = "W9lzyoRA24KPZMteIVI9AnQY7xdz1AAOFh8A/PKmND8A64XUVR/ZioHfsHIKTZvBXJlcrbJI6FTdvbUAwEmXPDAiwhcqes/N1+fNqG1m16J3atNtDASzUoI0/ZEZmI8l86JGI7o8ebLK"

    private fun state(keyEpoch: Long, vararg keys: Pair<Long, String>, rekeyRequired: Boolean = false) =
        CommunityKeyState("c", keyEpoch, rekeyRequired, keys.map { SealedEpochKey(it.first, it.second) })

    @Test
    fun buildsTheSameBoundPayloadAsTheWeb() {
        assertEquals(
            "5256434b01010203043f2a9c1e7b4d4e8a9c2f1a2b3c4d5e6fc8c3beb9b4afaaa5a09b96918c87827d78736e69645f5a55504b46413c37322d",
            hex(crypto.epochKeyPayload(k1, 0x01020304, community)),
        )
        // The largest epoch, and a community id written in capitals: still the same bytes.
        assertEquals(
            "5256434b01ffffffff9e8d7c6b5a494382a1b0c9d8e7f6a5b45a5b58595e5f5c5d52535051565754554a4b48494e4f4c4d4243404146474445",
            hex(crypto.epochKeyPayload(kMax, 0xffffffffL, other.uppercase())),
        )
        assertThrows(EpochKeyException::class.java) { crypto.epochKeyPayload(k0, -1, community) }
        assertThrows(EpochKeyException::class.java) { crypto.epochKeyPayload(k0, 0x1_0000_0000L, community) }
        assertThrows(EpochKeyException::class.java) { crypto.epochKeyPayload(k0.copyOf(31), 0, community) }
        assertThrows(EpochKeyException::class.java) { crypto.epochKeyPayload(k0, 0, "not-a-uuid") }
    }

    @Test
    fun opensEpochKeysTheWebSealed() {
        assertArrayEquals(k0, crypto.openEpochKey(b64(sealed0), 0, community, member))
        assertArrayEquals(k1, crypto.openEpochKey(b64(sealed1), 1, community, member))
        assertArrayEquals(kMax, crypto.openEpochKey(b64(sealedMax), 0xffffffffL, community, member))
        // 105 bytes on the wire: 57 bytes of payload plus crypto_box_seal's 48.
        assertEquals(105, b64(sealed0).size)
    }

    @Test
    fun refusesAnyOtherBinding() {
        assertThrows(EpochKeyException::class.java) { crypto.openEpochKey(b64(sealed1), 2, community, member) }
        assertThrows(EpochKeyException::class.java) { crypto.openEpochKey(b64(sealed1), 1, other, member) }
        // Sealed to someone else.
        assertThrows(EpochKeyException::class.java) { crypto.openEpochKey(b64(sealed1), 1, community, waiting) }
        // A plain group key (no binding) isn't accepted as a community key.
        assertThrows(EpochKeyException::class.java) { crypto.openEpochKey(b64(plainGroupKey), 0, community, member) }
        assertThrows(EpochKeyException::class.java) { crypto.openEpochKey(ByteArray(10), 0, community, member) }
    }

    @Test
    fun decryptsAMessageTheWebSentUnderEpochOne() {
        val ring = crypto.buildKeyRing(community, state(1, 0L to sealed0, 1L to sealed1), member)
        assertEquals(listOf(0L, 1L), ring.keys.keys.toList())
        assertTrue(ring.rejected.isEmpty())
        assertEquals(messageText, crypto.decrypt(ring, 1, b64(messageCiphertext), b64(messageNonce)))
        // Under the wrong epoch, or an epoch this member was never given, it's simply unreadable.
        assertNull(crypto.decrypt(ring, 0, b64(messageCiphertext), b64(messageNonce)))
        assertNull(crypto.decrypt(ring, 7, b64(messageCiphertext), b64(messageNonce)))
    }

    @Test
    fun opensTheWebsRotationAndEnrollment() {
        val next = crypto.openEpochKey(b64(rotationWrapped), 2, community, member)
        assertArrayEquals(rotationKey, next)
        assertFalse(next.contentEquals(k0) || next.contentEquals(k1))
        // The waiting member gets the current epoch's key (1), not a new one.
        assertArrayEquals(k1, crypto.openEpochKey(b64(enrollmentWrapped), 1, community, waiting))
    }

    @Test
    fun rejectsAReplayedCopyAReusedKeyAndAForeignOne() {
        // Old copy offered as epoch 1: the binding says 0.
        val replayed = crypto.buildKeyRing(community, state(1, 0L to sealed0, 1L to sealed0), member)
        assertFalse(replayed.keys.containsKey(1L))
        assertEquals(listOf(1L), replayed.rejected)
        assertThrows(EpochKeyException::class.java) { crypto.encrypt(replayed, "x") }
        // Same key re-sealed as epoch 1 by a malicious rotator: refused too.
        val reused = crypto.buildKeyRing(community, state(1, 0L to sealed0, 1L to resealedK0AsEpoch1), member)
        assertEquals(listOf(1L), reused.rejected)
        // Another community's key isn't accepted here, and garbage doesn't break the ring.
        assertTrue(crypto.buildKeyRing(community, state(0, 0L to foreign0), member).keys.isEmpty())
        assertEquals(listOf(0L), crypto.buildKeyRing(community, state(0, 0L to "not base64!"), member).rejected)
    }

    @Test
    fun aRoundTripOnThisSideMatchesTheWebsRules() {
        val creator = e2e.generateKeyPair()
        val zero = crypto.newCommunityKey(community, creator.publicKey)
        val creatorRing = crypto.buildKeyRing(community, state(0, 0L to zero.wrappedKey), creator)
        assertArrayEquals(zero.key, creatorRing.keys[0L])
        val memberB64 = Base64Codec.encode(member.publicKey)

        // Enrollment seals the current key; nothing is enrolled while a rotation is pending.
        val enrollments = crypto.prepareEnrollments(creatorRing, listOf(KeyRecipient("member", memberB64)))
        assertEquals(0L, enrollments.single().epoch)
        assertArrayEquals(zero.key, crypto.openEpochKey(b64(enrollments.single().wrappedKey), 0, community, member))
        val pending = crypto.buildKeyRing(community, state(0, 0L to zero.wrappedKey, rekeyRequired = true), creator)
        assertTrue(crypto.prepareEnrollments(pending, listOf(KeyRecipient("member", memberB64))).isEmpty())

        // A rotation seals a fresh key for epoch n+1 to exactly the roster.
        val rotation = crypto.prepareRotation(creatorRing, 0, listOf(KeyRecipient("creator", Base64Codec.encode(creator.publicKey)), KeyRecipient("member", memberB64)))
        assertEquals(0L, rotation.fromEpoch)
        assertEquals(listOf("creator", "member"), rotation.keys.map { it.userId })
        assertFalse(rotation.key.contentEquals(zero.key))
        val memberRing = crypto.buildKeyRing(community, state(1, 0L to enrollments.single().wrappedKey, 1L to rotation.keys[1].wrappedKey), member)
        val sent = crypto.encrypt(crypto.buildKeyRing(community, state(1, 0L to zero.wrappedKey, 1L to rotation.keys[0].wrappedKey), creator), "after")
        assertEquals(1L, sent.keyEpoch)
        assertEquals("after", crypto.decrypt(memberRing, sent.keyEpoch, sent.ciphertext, sent.nonce))
    }
}

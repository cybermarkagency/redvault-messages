// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.SigningKeyPair
import com.redvaultsystems.redvaultmessage.data.api.KeyNoticeDto
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.File

/** Mirrors ui/src/test/keyTrust.test.ts: the attacks from audit/tests EXPOSE-2 and finding H-2, refused, with controls. */
class KeyTrustTest {
    private val crypto = E2eCrypto(LazySodiumJava(SodiumJava()))
    private val self = "11111111-1111-4111-8111-111111111111"
    private val bob = "22222222-2222-4222-8222-222222222222"
    private val eve = "33333333-3333-4333-8333-333333333333"
    private val dm = "44444444-4444-4444-8444-444444444444"
    private val group = "55555555-5555-4555-8555-555555555555"

    private val pins = MemoryKeyPins()
    private val trust = KeyTrust(crypto, pins)
    private val signers: Map<String, SigningKeyPair> = listOf(self, bob, eve).associateWith { crypto.deriveSigningKeyPair(crypto.generateConversationKey()) }

    private fun key() = crypto.generateConversationKey()

    private fun notice(issuer: String, epoch: Long, seq: Long, members: List<String>, key: ByteArray, signingAs: String = issuer): KeyNoticeDto {
        val input = E2eCrypto.keyNoticeInput(group, issuer, epoch, seq, members, crypto.keyCommitment(key))
        return KeyNoticeDto(epoch, seq, issuer, members, Base64Codec.encode(crypto.signMessage(input, signers.getValue(signingAs).secretKey)))
    }

    private fun check(key: ByteArray?, notice: KeyNoticeDto?, epoch: Int = notice?.epoch?.toInt() ?: 0): String? =
        trust.checkGroupKey(GroupKeyCheck(group, self, epoch, key, notice) { id -> signers[id]?.publicKey })

    // --- Forged answers ---

    @Test
    fun theSamePinHoldsForAnotherSpellingOfTheIds() {
        val id = "abababab-abab-4aba-8aba-abababababab"
        assertNull(trust.checkDirectKey(self, id, 0, key()))
        assertEquals(KeyTrust.DIRECT_KEY_CHANGED, trust.checkDirectKey(self, id.uppercase(), 0, key()))
        assertEquals(KeyTrust.DIRECT_KEY_CHANGED, trust.checkDirectKey(self.uppercase(), id, 0, key()))
    }

    @Test
    fun theSameSequenceNumberWithOtherMembersIsRefused() {
        val k1 = key()
        check(k1, notice(self, 0, 1, listOf(self, bob), k1))
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(k1, notice(bob, 0, 1, listOf(self, bob, eve), k1)))
        assertNull(check(k1, notice(self, 0, 1, listOf(self, bob), k1)))
    }

    @Test
    fun aNoticeCantBeMadeWithoutTheKeyEvenWithASigningKeyOfOnesOwn() {
        val k1 = key()
        val forged = notice(eve, 0, 9, listOf(bob, eve), key())
        val signingKey = { id: String -> signers[id]?.publicKey }
        assertEquals(false, trust.noticeSignatureOk(group, forged, k1, signingKey))
        assertEquals(true, trust.noticeSignatureOk(group, notice(bob, 0, 9, listOf(bob), k1), k1, signingKey))
        assertEquals(false, trust.noticeSignatureOk(group, notice(bob, 0, 9, listOf(bob), k1), k1) { null })
    }

    @Test
    fun aServerChosenPublicKeyDoesntMatchThePrivateKey() {
        val pair = crypto.generateKeyPair()
        assertEquals(true, crypto.publicKeyOf(pair.privateKey).contentEquals(pair.publicKey))
        assertEquals(false, crypto.publicKeyOf(pair.privateKey).contentEquals(crypto.generateKeyPair().publicKey))
    }

    // --- Who a notice names ---

    @Test
    fun aNoticeNamesTheVerifiedMembersNotTheServersList() {
        val k1 = key()
        check(k1, notice(self, 0, 1, listOf(self, bob), k1))
        assertEquals(listOf(self, bob, dm).sorted(), trust.membersToSign(self, group, listOf(self, bob, eve), listOf(dm)).sorted())
        assertEquals(listOf(self), trust.membersToSign(self, group, listOf(self), emptyList()))
    }

    @Test
    fun someoneJoiningNamesTheLastNoticeNotTheServersList() {
        val k1 = key()
        val latest = notice(bob, 0, 1, listOf(bob, group), k1)
        assertEquals(listOf(self, bob, group).sorted(), trust.membersToSign(self, group, listOf(self, bob, group, eve), listOf(self), latest).sorted())
    }

    @Test
    fun controlWithNothingVerifiedTheServersListIsAllThereIs() {
        assertEquals(listOf(self, bob), trust.membersToSign(self, group, listOf(self, bob), emptyList()).sorted())
    }

    @Test
    fun anOlderEpochThisPhoneVerifiedCantBeSwappedLater() {
        val k1 = key()
        val k2 = key()
        check(k1, notice(self, 0, 1, listOf(self, bob), k1))
        check(k2, notice(bob, 1, 1, listOf(self, bob), k2))
        assertEquals(true, trust.matchesVerifiedEpoch(self, group, 0, k1))
        assertEquals(false, trust.matchesVerifiedEpoch(self, group, 0, key()))
        assertEquals(true, trust.matchesVerifiedEpoch(self, group, 7, key()))
    }

    // --- Direct chats ---

    @Test
    fun aDifferentKeyAtTheSameEpochIsRefused() {
        assertNull(trust.checkDirectKey(self, dm, 0, key()))
        assertEquals(KeyTrust.DIRECT_KEY_CHANGED, trust.checkDirectKey(self, dm, 0, key()))
    }

    @Test
    fun aLaterEpochIsRefusedEvenWithTheSameKey() {
        val key = key()
        assertNull(trust.checkDirectKey(self, dm, 0, key))
        assertEquals(KeyTrust.DIRECT_KEY_CHANGED, trust.checkDirectKey(self, dm, 1, key))
    }

    @Test
    fun aRefusedKeyDoesNotReplaceThePin() {
        val first = key()
        trust.checkDirectKey(self, dm, 0, first)
        trust.checkDirectKey(self, dm, 0, key())
        assertNull(trust.checkDirectKey(self, dm, 0, first))
    }

    @Test
    fun aKeyThatWontOpenIsSaidAndPinsNothing() {
        assertEquals(KeyTrust.DIRECT_KEY_UNOPENED, trust.checkDirectKey(self, dm, 0, null))
        assertEquals(KeyTrust.DIRECT_KEY_UNOPENED, trust.checkDirectKey(self, dm, 0, ByteArray(16)))
        assertNull(pins.load(self, dm))
    }

    @Test
    fun controlTheSameKeyKeepsWorkingAndPinsAreKeptApart() {
        val key = key()
        assertNull(trust.checkDirectKey(self, dm, 0, key))
        assertNull(trust.checkDirectKey(self, dm, 0, key))
        // Another account on this phone, and another chat, start fresh.
        assertNull(trust.checkDirectKey(bob, dm, 0, key()))
        assertNull(trust.checkDirectKey(self, group, 0, key()))
    }

    // --- Group key notices ---

    @Test
    fun controlASignedNoticeIsAcceptedThenANewEpochFromAMember() {
        val k1 = key()
        assertNull(check(k1, notice(self, 0, 1, listOf(self, bob), k1)))
        val k2 = key()
        assertNull(check(k2, notice(bob, 1, 1, listOf(self, bob), k2)))
        assertEquals(1L, pins.load(self, group)?.epoch)
    }

    @Test
    fun aServerChosenKeyIsRefusedNoRealMemberSignedIt() {
        val k1 = key()
        check(k1, notice(self, 0, 1, listOf(self, bob), k1))
        val forged = notice(bob, 1, 1, listOf(self, bob), k1) // Signed for another key.
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(key(), forged))
    }

    @Test
    fun aSignatureByTheWrongPersonIsRefused() {
        val k1 = key()
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(k1, notice(bob, 0, 1, listOf(self, bob), k1, signingAs = eve)))
        assertNull(pins.load(self, group))
    }

    @Test
    fun aNewEpochFromSomeoneOutsideTheGroupIsRefusedEvenWithAValidSignature() {
        val k1 = key()
        check(k1, notice(self, 0, 1, listOf(self, bob), k1))
        val k2 = key()
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(k2, notice(eve, 1, 1, listOf(self, bob, eve), k2)))
    }

    @Test
    fun aHiddenMemberCantBeAddedToASignedNotice() {
        val k1 = key()
        val tampered = notice(self, 0, 1, listOf(self, bob), k1).copy(members = listOf(self, bob, eve))
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(k1, tampered))
    }

    @Test
    fun aNoticeForAnotherEpochOrSeqThanSignedIsRefused() {
        val k1 = key()
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(k1, notice(self, 0, 1, listOf(self, bob), k1).copy(seq = 2)))
        val signed = notice(self, 2, 1, listOf(self, bob), k1)
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(k1, signed, epoch = 3))
    }

    @Test
    fun rollbackOfEpochOrSeqAndARemovedNoticeAreRefused() {
        val k1 = key()
        assertNull(check(k1, notice(self, 0, 2, listOf(self, bob), k1)))
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(k1, notice(self, 0, 1, listOf(self, bob), k1)))
        val k2 = key()
        assertNull(check(k2, notice(bob, 1, 1, listOf(self, bob), k2)))
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(k1, notice(self, 0, 3, listOf(self, bob), k1)))
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(k2, null, epoch = 1))
    }

    @Test
    fun aDifferentKeyUnderAnEpochAlreadySeenIsRefused() {
        val k1 = key()
        check(k1, notice(self, 0, 1, listOf(self, bob), k1))
        val other = key()
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(other, notice(bob, 0, 2, listOf(self, bob), other)))
    }

    @Test
    fun aNoticeThatLeavesThisPersonOutOrItsIssuerIsRefused() {
        val k1 = key()
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(k1, notice(bob, 0, 1, listOf(bob, eve), k1)))
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, check(k1, notice(bob, 0, 1, listOf(self, eve), k1)))
    }

    @Test
    fun anIssuerWithNoSigningKeyIsNotTrusted() {
        val k1 = key()
        val problem = trust.checkGroupKey(GroupKeyCheck(group, self, 0, k1, notice(bob, 0, 1, listOf(self, bob), k1)) { null })
        assertEquals(KeyTrust.GROUP_KEY_UNVERIFIED, problem)
        assertNull(pins.load(self, group))
    }

    @Test
    fun aKeyThatWontOpenIsSaid() {
        assertEquals(KeyTrust.GROUP_KEY_UNOPENED, check(null, notice(self, 0, 1, listOf(self, bob), key())))
    }

    @Test
    fun controlAGroupWithNoNoticeYetWorksUntilOneHasBeenSeen() {
        assertNull(check(key(), null))
        assertNull(pins.load(self, group))
    }

    @Test
    fun aFailedCheckPinsNothing() {
        val k1 = key()
        check(k1, notice(self, 0, 1, listOf(self, bob), k1))
        val before = pins.load(self, group)
        check(key(), notice(eve, 5, 9, listOf(self, eve), key()))
        assertEquals(before, pins.load(self, group))
    }

    @Test
    fun membersNoNoticeNamedAreFlaggedSoARotationWontSealTheKeyToThem() {
        val k1 = key()
        assertEquals(emptyList<String>(), trust.unconfirmedMembers(self, group, listOf(self, bob, eve)))
        check(k1, notice(self, 0, 1, listOf(self, bob), k1))
        assertEquals(listOf(eve), trust.unconfirmedMembers(self, group, listOf(self, bob, eve)))
        assertEquals(emptyList<String>(), trust.unconfirmedMembers(self, group, listOf(self, bob)))
        assertEquals(emptyList<String>(), trust.unconfirmedMembers(self, group, listOf(self.uppercase(), bob)))
    }

    @Test
    fun aDirectChatsPinNamesNobodySoItNeverFlagsMembers() {
        trust.checkDirectKey(self, dm, 0, key())
        assertEquals(emptyList<String>(), trust.unconfirmedMembers(self, dm, listOf(self, bob)))
    }

    @Test
    fun theNextSeqGoesPastBothThePinAndTheServersNotice() {
        val k1 = key()
        assertEquals(1L, trust.nextSeq(self, group, 0, null))
        check(k1, notice(self, 0, 4, listOf(self, bob), k1))
        assertEquals(5L, trust.nextSeq(self, group, 0, null))
        assertEquals(10L, trust.nextSeq(self, group, 0, notice(bob, 0, 9, listOf(self, bob), k1)))
        // A new epoch starts over, whatever an older one reached.
        assertEquals(1L, trust.nextSeq(self, group, 1, notice(bob, 0, 9, listOf(self, bob), k1)))
    }

    // --- Shared vectors, byte for byte ---

    @Test
    fun commitmentAndSignedBytesMatchWhatEveryClientMustProduce() {
        val vectors = Json.parseToJsonElement(File("../../desktop/tests/vectors/key_notice_vectors.json").readText()).jsonObject
        val cases = vectors.getValue("cases").jsonArray.map { it.jsonObject }
        assertEquals(3, cases.size)
        for (case in cases) {
            val notice = case.getValue("notice").jsonObject
            val commitment = crypto.keyCommitment(Base64Codec.decode(case.getValue("key").jsonPrimitive.content))
            assertEquals(case.getValue("commitment").jsonPrimitive.content, Base64Codec.encode(commitment))
            val input = E2eCrypto.keyNoticeInput(
                case.getValue("conversationId").jsonPrimitive.content,
                notice.getValue("issuerId").jsonPrimitive.content,
                notice.getValue("epoch").jsonPrimitive.long,
                notice.getValue("seq").jsonPrimitive.long,
                notice.getValue("members").jsonArray.map { it.jsonPrimitive.content },
                commitment,
            )
            assertEquals(case.getValue("input").jsonPrimitive.content, Base64Codec.encode(input))
        }
    }

    @Test
    fun theSignedBytesDontDependOnMemberOrderOrCase() {
        val commitment = crypto.keyCommitment(key())
        val one = E2eCrypto.keyNoticeInput(group, self, 1, 1, listOf(self, bob), commitment)
        val two = E2eCrypto.keyNoticeInput(group.uppercase(), self.uppercase(), 1, 1, listOf(bob.uppercase(), self), commitment)
        assertEquals(Base64Codec.encode(one), Base64Codec.encode(two))
    }
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import java.security.MessageDigest

// Port of ui/src/lib/communityCrypto.ts: community group keys with epochs
// (docs/paid-communities.md, "Encrypted membership"). Same primitives as every
// other group here (a random XChaCha20-Poly1305 key, sealed to each member with
// crypto_box_seal) plus one rule: whenever anyone leaves or is removed, a
// remaining member's app makes a NEW key (the next epoch) and seals it only to
// who remains. Nothing is sent under an older epoch afterwards (the server
// refuses it), so a removed member's saved keys open nothing new.
//
// Each sealed copy binds the key to its community and epoch:
//   "RVCK" | version 1 | epoch (u32, big-endian) | community id (16 bytes) | key (32 bytes)
// so the server can't hand someone an old epoch's copy as a new epoch, or
// another community's key: the app checks both before using a key, and
// refuses a "new" epoch whose key equals one it already holds.
//
// Byte-compatible with the web: test/.../CommunityCryptoInteropTest.kt opens
// copies the web sealed, and ui/src/test/communityCryptoInterop.test.ts opens
// copies this file sealed.

class EpochKeyException(message: String) : Exception(message)

/** One sealed epoch key as the server lists it. */
data class SealedEpochKey(val epoch: Long, val wrappedKey: String)

/** What GET /communities/:id/keys returns: this member's sealed copies of every epoch they were given. */
data class CommunityKeyState(
    val conversationId: String,
    val keyEpoch: Long,
    val rekeyRequired: Boolean,
    val keys: List<SealedEpochKey>,
)

/** Someone to seal a key to: a member on the roster, or one waiting to be let in. */
data class KeyRecipient(val userId: String, val publicKey: String)

/** A sealed copy for one member (a rotation's keys). */
data class MemberKey(val userId: String, val wrappedKey: String)

/** The current epoch's key for a member who is waiting to be let in. */
data class Enrollment(val userId: String, val epoch: Long, val wrappedKey: String)

class CommunityCiphertext(val ciphertext: ByteArray, val nonce: ByteArray, val keyEpoch: Long)

/** The next epoch: [key] sealed to everyone on the roster. */
class PreparedRotation(val fromEpoch: Long, val key: ByteArray, val keys: List<MemberKey>)

/** Epoch 0, made by the creator's app for a community id it chose. */
class NewCommunityKey(val key: ByteArray, val wrappedKey: String)

/** Every epoch key this member holds for one community. */
class KeyRing(
    val communityId: String,
    val conversationId: String,
    /** The epoch new messages must use. */
    val currentEpoch: Long,
    val rekeyRequired: Boolean,
    val keys: Map<Long, ByteArray>,
    /** Epochs whose sealed copy failed the checks (tampering or corruption): their messages show as unreadable. */
    val rejected: List<Long>,
) {
    /** Overwrites the keys. Call when signing out. */
    fun wipe() = keys.values.forEach { it.fill(0) }
}

class CommunityCrypto(private val e2e: E2eCrypto) {

    /** The bound payload for one epoch key (public for tests). */
    fun epochKeyPayload(key: ByteArray, epoch: Long, communityId: String): ByteArray {
        if (key.size != KEY_BYTES) throw EpochKeyException("community keys are 32 bytes")
        if (epoch < 0 || epoch > MAX_EPOCH) throw EpochKeyException("bad epoch")
        val out = ByteArray(PAYLOAD_BYTES)
        MAGIC.copyInto(out, 0)
        out[4] = VERSION
        out[5] = (epoch ushr 24).toByte()
        out[6] = (epoch ushr 16).toByte()
        out[7] = (epoch ushr 8).toByte()
        out[8] = epoch.toByte()
        uuidBytes(communityId).copyInto(out, 9)
        key.copyInto(out, 25)
        return out
    }

    /** Seals one epoch's key to one member's public key. */
    fun sealEpochKey(key: ByteArray, epoch: Long, communityId: String, recipientPublicKey: ByteArray): ByteArray {
        val payload = epochKeyPayload(key, epoch, communityId)
        try {
            return e2e.sealConversationKey(payload, recipientPublicKey)
        } finally {
            payload.fill(0)
        }
    }

    /** Opens a sealed epoch key, checking it was made for exactly this community and epoch. */
    fun openEpochKey(sealed: ByteArray, epoch: Long, communityId: String, identity: KeyPair): ByteArray {
        val payload = try {
            e2e.unsealConversationKey(sealed, identity.publicKey, identity.privateKey)
        } catch (_: CryptoException) {
            throw EpochKeyException("This key wasn't sealed to you.")
        }
        try {
            if (payload.size != PAYLOAD_BYTES || !payload.copyOfRange(0, 4).contentEquals(MAGIC) || payload[4] != VERSION) {
                throw EpochKeyException("Not a community key.")
            }
            val sealedEpoch = ((payload[5].toLong() and 0xff) shl 24) or ((payload[6].toLong() and 0xff) shl 16) or
                ((payload[7].toLong() and 0xff) shl 8) or (payload[8].toLong() and 0xff)
            if (sealedEpoch != epoch) throw EpochKeyException("Key sealed for a different epoch.")
            if (!MessageDigest.isEqual(payload.copyOfRange(9, 25), uuidBytes(communityId))) {
                throw EpochKeyException("Key sealed for a different community.")
            }
            return payload.copyOfRange(25, PAYLOAD_BYTES)
        } finally {
            payload.fill(0)
        }
    }

    fun buildKeyRing(communityId: String, state: CommunityKeyState, identity: KeyPair): KeyRing {
        val keys = LinkedHashMap<Long, ByteArray>()
        val rejected = ArrayList<Long>()
        for (k in state.keys.sortedBy { it.epoch }) {
            try {
                val key = openEpochKey(Base64Codec.decode(k.wrappedKey), k.epoch, communityId, identity)
                // A "new" epoch with a key we already hold would let whoever held the old one read on.
                if (keys.values.any { MessageDigest.isEqual(it, key) }) {
                    key.fill(0)
                    throw EpochKeyException("Reused key")
                }
                keys[k.epoch] = key
            } catch (_: Exception) {
                rejected += k.epoch
            }
        }
        return KeyRing(communityId, state.conversationId, state.keyEpoch, state.rekeyRequired, keys, rejected)
    }

    /** Encrypts a body under the current epoch. Throws when this member doesn't hold that key (yet). */
    fun encrypt(ring: KeyRing, plaintext: String): CommunityCiphertext {
        val key = ring.keys[ring.currentEpoch] ?: throw EpochKeyException("You don't have the community's current key yet.")
        val sealed = e2e.encryptWithConversationKey(plaintext, key)
        return CommunityCiphertext(sealed.ciphertext, sealed.nonce, ring.currentEpoch)
    }

    /** Null when the epoch's key isn't held (from before joining) or the body doesn't open. */
    fun decrypt(ring: KeyRing, epoch: Long, ciphertext: ByteArray, nonce: ByteArray): String? {
        val key = ring.keys[epoch] ?: return null
        return try {
            e2e.decryptWithConversationKey(ciphertext, nonce, key)
        } catch (_: Exception) {
            null
        }
    }

    /**
     * The next epoch: a fresh random key sealed to every member on the roster
     * (who the server says is still in). Never one of the keys already held.
     */
    fun prepareRotation(ring: KeyRing, rosterEpoch: Long, members: List<KeyRecipient>): PreparedRotation {
        var key = e2e.generateConversationKey()
        while (ring.keys.values.any { MessageDigest.isEqual(it, key) }) key = e2e.generateConversationKey()
        val epoch = rosterEpoch + 1
        return PreparedRotation(
            fromEpoch = rosterEpoch,
            key = key,
            keys = members.map { m -> MemberKey(m.userId, Base64Codec.encode(sealEpochKey(key, epoch, ring.communityId, Base64Codec.decode(m.publicKey)))) },
        )
    }

    /** The current epoch's key, sealed to each member who is waiting to be let in. */
    fun prepareEnrollments(ring: KeyRing, pending: List<KeyRecipient>): List<Enrollment> {
        val key = ring.keys[ring.currentEpoch]
        if (key == null || ring.rekeyRequired) return emptyList()
        return pending.map { p ->
            Enrollment(p.userId, ring.currentEpoch, Base64Codec.encode(sealEpochKey(key, ring.currentEpoch, ring.communityId, Base64Codec.decode(p.publicKey))))
        }
    }

    /** Epoch 0, made by the creator's app for a community id it chose. */
    fun newCommunityKey(communityId: String, creatorPublicKey: ByteArray): NewCommunityKey {
        val key = e2e.generateConversationKey()
        return NewCommunityKey(key, Base64Codec.encode(sealEpochKey(key, 0, communityId, creatorPublicKey)))
    }

    companion object {
        private val MAGIC = byteArrayOf(0x52, 0x56, 0x43, 0x4b) // "RVCK"
        private const val VERSION: Byte = 1
        const val KEY_BYTES = 32
        const val PAYLOAD_BYTES = 4 + 1 + 4 + 16 + KEY_BYTES
        const val MAX_EPOCH = 0xffffffffL
        private val UUID_HEX = Regex("^[0-9a-f]{32}$")

        private fun uuidBytes(id: String): ByteArray {
            val hex = id.replace("-", "").lowercase()
            if (!UUID_HEX.matches(hex)) throw EpochKeyException("not a community id")
            return ByteArray(16) { i -> hex.substring(i * 2, i * 2 + 2).toInt(16).toByte() }
        }
    }
}

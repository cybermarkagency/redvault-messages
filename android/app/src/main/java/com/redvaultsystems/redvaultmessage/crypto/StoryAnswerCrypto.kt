package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium
import com.redvaultsystems.redvaultmessage.data.model.AnswerException
import com.redvaultsystems.redvaultmessage.data.model.AnswerableSticker
import com.redvaultsystems.redvaultmessage.data.model.StickerTotals
import com.redvaultsystems.redvaultmessage.data.model.StickerTotalsParser
import com.redvaultsystems.redvaultmessage.data.model.StoryAnswer
import com.redvaultsystems.redvaultmessage.data.model.StoryAnswers
import kotlinx.serialization.json.Json
import java.nio.ByteBuffer

// Answers to stickers (viewer to author) and totals (author to everyone who can see the story): port of the second half of
// ui/src/lib/storyCrypto.ts, with the formats of docs/stories-advanced/architecture.md.
//
// An answer is sealed so that only the story's author can read it:
//   named ("RVSR"):             nonce(24) || crypto_box(payload, author public key, responder private key). The author knows who
//                               sent it (their key made it), and so does the sender, who can open it again with the author's key;
//   without a name ("RVSA"):    crypto_box_seal(payload, author public key). No sender key inside at all.
// payload = magic (4) | version 1 | story id (16) | sticker id length (1) | sticker id | answer JSON. The story and sticker ids
// inside stop a server moving an answer onto another story or sticker.
//
// Totals are aggregates only, sealed with the story key (XChaCha20-Poly1305), additional data
// "redvault-story-result-v1" || story id || revision (4 bytes, big-endian) || sticker id, so a server can't forge them or serve
// them for another sticker. It can still serve an older revision.
class StoryAnswerCrypto(private val sodium: LazySodium) {

    /** An answer the author knows came from this account. The sender can open it again too ([openAnswer] with the author's public key). */
    fun sealAnswer(storyId: String, sticker: AnswerableSticker, answer: StoryAnswer, authorPublicKey: ByteArray, responderPrivateKey: ByteArray): ByteArray {
        val payload = payload(NAMED_MAGIC, storyId, sticker.id, checked(answer, sticker))
        val nonce = sodium.randomBytesBuf(BOX_NONCE_BYTES)
        val box = ByteArray(payload.size + BOX_MAC_BYTES)
        if (!sodium.cryptoBoxEasy(box, payload, payload.size.toLong(), nonce, authorPublicKey, responderPrivateKey)) throw StoryCryptoException("Failed to seal the answer")
        return nonce + box
    }

    /**
     * Opens a named answer. The author passes the responder's public key and their own private key; the responder passes the
     * author's public key and their own private key.
     */
    fun openAnswer(storyId: String, sticker: AnswerableSticker, envelope: ByteArray, otherPartyPublicKey: ByteArray, ownPrivateKey: ByteArray): StoryAnswer {
        if (envelope.size <= BOX_NONCE_BYTES + BOX_MAC_BYTES) throw AnswerException()
        val nonce = envelope.copyOfRange(0, BOX_NONCE_BYTES)
        val box = envelope.copyOfRange(BOX_NONCE_BYTES, envelope.size)
        val payload = ByteArray(box.size - BOX_MAC_BYTES)
        if (!sodium.cryptoBoxOpenEasy(payload, box, box.size.toLong(), nonce, otherPartyPublicKey, ownPrivateKey)) {
            throw StoryCryptoException("this answer wasn't made by that person for this author")
        }
        return openPayload(payload, NAMED_MAGIC, storyId, sticker)
    }

    /** An answer with no sender inside (a question marked "without my name"). */
    fun sealAnonymousAnswer(storyId: String, sticker: AnswerableSticker, answer: StoryAnswer, authorPublicKey: ByteArray): ByteArray {
        val payload = payload(ANON_MAGIC, storyId, sticker.id, checked(answer, sticker))
        val sealed = ByteArray(payload.size + SEAL_BYTES)
        if (!sodium.cryptoBoxSeal(sealed, payload, payload.size.toLong(), authorPublicKey)) throw StoryCryptoException("Failed to seal the answer")
        return sealed
    }

    fun openAnonymousAnswer(storyId: String, sticker: AnswerableSticker, envelope: ByteArray, authorPublicKey: ByteArray, authorPrivateKey: ByteArray): StoryAnswer {
        if (envelope.size <= SEAL_BYTES) throw StoryCryptoException("this answer isn't for this author")
        val payload = ByteArray(envelope.size - SEAL_BYTES)
        if (!sodium.cryptoBoxSealOpen(payload, envelope, envelope.size.toLong(), authorPublicKey, authorPrivateKey)) {
            throw StoryCryptoException("this answer isn't for this author")
        }
        return openPayload(payload, ANON_MAGIC, storyId, sticker)
    }

    /** Seals the totals of one sticker with the story's key. [nonce] is random unless a test pins it. */
    fun sealTotals(
        storyId: String,
        stickerId: String,
        revision: Int,
        totals: StickerTotals,
        storyKey: ByteArray,
        nonce: ByteArray = sodium.randomBytesBuf(AEAD_NONCE_BYTES),
    ): AeadCiphertext {
        if (revision < 1) throw StoryCryptoException("bad revision")
        val plain = StickerTotalsParser.toJson(totals).toByteArray(Charsets.UTF_8)
        val ad = resultAd(storyId, stickerId, revision)
        val cipher = ByteArray(plain.size + AEAD_TAG_BYTES)
        val len = LongArray(1)
        if (!sodium.cryptoAeadXChaCha20Poly1305IetfEncrypt(cipher, len, plain, plain.size.toLong(), ad, ad.size.toLong(), null, nonce, storyKey)) {
            throw StoryCryptoException("AEAD encryption failed")
        }
        return AeadCiphertext(cipher.copyOf(len[0].toInt()), nonce)
    }

    fun openTotals(storyId: String, sticker: AnswerableSticker, revision: Int, ciphertext: ByteArray, nonce: ByteArray, storyKey: ByteArray): StickerTotals {
        if (ciphertext.size < AEAD_TAG_BYTES || nonce.size != AEAD_NONCE_BYTES || storyKey.size != StoryCrypto.KEY_BYTES) throw StoryCryptoException("these results can't be opened")
        val ad = resultAd(storyId, sticker.id, revision)
        val plain = ByteArray(ciphertext.size - AEAD_TAG_BYTES)
        val len = LongArray(1)
        if (!sodium.cryptoAeadXChaCha20Poly1305IetfDecrypt(plain, len, null, ciphertext, ciphertext.size.toLong(), ad, ad.size.toLong(), nonce, storyKey)) {
            throw StoryCryptoException("these results can't be opened")
        }
        val json = try {
            Json.parseToJsonElement(String(plain.copyOf(len[0].toInt()), Charsets.UTF_8))
        } catch (e: Exception) {
            throw AnswerException("malformed results")
        }
        return StickerTotalsParser.parse(json, sticker)
    }

    private fun checked(answer: StoryAnswer, sticker: AnswerableSticker): StoryAnswer =
        StoryAnswers.parse(Json.parseToJsonElement(StoryAnswers.toJson(answer)), sticker)

    private fun payload(magic: ByteArray, storyId: String, stickerId: String, answer: StoryAnswer): ByteArray {
        if (!STICKER_ID.matches(stickerId)) throw StoryCryptoException("not a sticker id")
        val id = stickerId.toByteArray(Charsets.UTF_8)
        val body = StoryAnswers.toJson(answer).toByteArray(Charsets.UTF_8)
        return magic + byteArrayOf(ANSWER_VERSION) + StoryCrypto.uuidBytes(storyId) + byteArrayOf(id.size.toByte()) + id + body
    }

    private fun openPayload(payload: ByteArray, magic: ByteArray, storyId: String, sticker: AnswerableSticker): StoryAnswer {
        val idBytes = StoryCrypto.ID_BYTES
        if (payload.size < 6 + idBytes) throw AnswerException()
        if (!payload.copyOfRange(0, 4).contentEquals(magic) || payload[4] != ANSWER_VERSION) throw AnswerException("unknown answer format")
        if (!payload.copyOfRange(5, 5 + idBytes).contentEquals(StoryCrypto.uuidBytes(storyId))) throw AnswerException("this answer belongs to another story")
        val idLen = payload[5 + idBytes].toInt() and 0xff
        val idEnd = 6 + idBytes + idLen
        if (idEnd > payload.size || String(payload, 6 + idBytes, idLen, Charsets.UTF_8) != sticker.id) throw AnswerException("this answer belongs to another sticker")
        val json = try {
            Json.parseToJsonElement(String(payload, idEnd, payload.size - idEnd, Charsets.UTF_8))
        } catch (e: Exception) {
            throw AnswerException()
        }
        return StoryAnswers.parse(json, sticker)
    }

    private fun resultAd(storyId: String, stickerId: String, revision: Int): ByteArray {
        val id = stickerId.toByteArray(Charsets.UTF_8)
        return RESULT_AD.toByteArray(Charsets.UTF_8) + StoryCrypto.uuidBytes(storyId) + ByteBuffer.allocate(4).putInt(revision).array() + id
    }

    private companion object {
        val NAMED_MAGIC = byteArrayOf(0x52, 0x56, 0x53, 0x52) // "RVSR"
        val ANON_MAGIC = byteArrayOf(0x52, 0x56, 0x53, 0x41) // "RVSA"
        const val ANSWER_VERSION: Byte = 1
        const val BOX_NONCE_BYTES = 24
        const val BOX_MAC_BYTES = 16
        const val SEAL_BYTES = 48
        const val AEAD_NONCE_BYTES = 24
        const val AEAD_TAG_BYTES = 16
        const val RESULT_AD = "redvault-story-result-v1"
        val STICKER_ID = Regex("^[a-z0-9]{6,16}$")
    }
}

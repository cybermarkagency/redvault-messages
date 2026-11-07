package com.redvaultsystems.redvaultmessage.data

import android.content.Context
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.CryptoException
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.data.api.KeyNoticeDto
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

/**
 * What this phone last verified about a conversation's key: [commitment] is base64, [members] lowercase, and [past]
 * the commitments to the epochs before [epoch] that it verified, so they can't be swapped later.
 */
@Serializable
data class KeyPin(val epoch: Long, val seq: Long, val commitment: String, val members: List<String>, val past: Map<String, String> = emptyMap())

/** Key pins are per account and per phone. */
interface KeyPins {
    fun load(selfUserId: String, conversationId: String): KeyPin?
    fun save(selfUserId: String, conversationId: String, pin: KeyPin)
}

class KeyPinStore(context: Context) : KeyPins {
    private val prefs = context.getSharedPreferences("keyPins", Context.MODE_PRIVATE)

    override fun load(selfUserId: String, conversationId: String): KeyPin? = prefs.getString(name(selfUserId, conversationId), null)
        ?.let { runCatching { Json.decodeFromString(KeyPin.serializer(), it) }.getOrNull() }

    override fun save(selfUserId: String, conversationId: String, pin: KeyPin) {
        prefs.edit().putString(name(selfUserId, conversationId), Json.encodeToString(KeyPin.serializer(), pin)).apply()
    }

    private fun name(selfUserId: String, conversationId: String) = pinName(selfUserId, conversationId)
}

/** Lowercased, like every id compared here: another spelling must never find no pin and start over. */
private fun pinName(selfUserId: String, conversationId: String) = "${selfUserId.lowercase()}:${conversationId.lowercase()}"

/** Pins for one session, with nothing kept: for tests. */
class MemoryKeyPins : KeyPins {
    private val pins = java.util.concurrent.ConcurrentHashMap<String, KeyPin>()

    override fun load(selfUserId: String, conversationId: String): KeyPin? = pins[pinName(selfUserId, conversationId)]

    override fun save(selfUserId: String, conversationId: String, pin: KeyPin) {
        pins[pinName(selfUserId, conversationId)] = pin
    }
}

/** What [KeyTrust.checkGroupKey] is given about a group's current key. */
class GroupKeyCheck(
    val conversationId: String,
    val selfUserId: String,
    val epoch: Int,
    /** The key the server's copy opens to, for [epoch]. */
    val key: ByteArray?,
    val notice: KeyNoticeDto?,
    /** The pinned signing key of a user, or null when unknown. */
    val signingKey: (String) -> ByteArray?,
)

/**
 * Trust in protocol 1 conversation keys: port of ui/src/lib/keyTrust.ts
 * (remediation/SECURITY_DESIGN_AND_MIGRATION.md, C-2 and H-2).
 *
 * crypto_box_seal says nothing about who sealed a key, so the server could hand out a key of its own. Two rules stop that:
 *   - A direct chat's key never changes: this phone pins the key it first saw, and a new epoch or a different key is refused.
 *   - A group's key comes with a key notice, signed by the member who issued it, naming the conversation, the epoch, the
 *     members and a commitment to the key. A new epoch's issuer must be in the member list this phone last verified,
 *     and epochs and sequence numbers never go backwards.
 *
 * First contact is still trust on first use: a phone has nothing earlier to compare with.
 */
class KeyTrust(private val crypto: E2eCrypto, private val pins: KeyPins, private val contactKeys: ContactKeyPins? = null) {

    /**
     * Call before sealing anything to someone. The server hands out public keys, so one that differs from what this
     * phone has seen would seal the group key, or a shared item, to whoever holds the other half.
     */
    fun requireUnchangedKey(selfUserId: String, contactUserId: String, publicKeyB64: String) {
        if (contactKeys?.keyChanged(selfUserId, contactUserId, publicKeyB64, verified = false) == true) {
            throw UserFacingException(CONTACT_KEY_CHANGED)
        }
    }

    /** What's wrong with a direct chat's key, or null. Pins the key the first time. */
    fun checkDirectKey(selfUserId: String, conversationId: String, epoch: Int, key: ByteArray?): String? {
        if (epoch != 0) return DIRECT_KEY_CHANGED
        if (key == null) return DIRECT_KEY_UNOPENED
        val commitment = commitmentOf(key) ?: return DIRECT_KEY_UNOPENED
        val pin = pins.load(selfUserId, conversationId)
        if (pin != null && pin.commitment != commitment) return DIRECT_KEY_CHANGED
        if (pin == null) pins.save(selfUserId, conversationId, KeyPin(0, 0, commitment, emptyList()))
        return null
    }

    fun isPinned(selfUserId: String, conversationId: String): Boolean = pins.load(selfUserId, conversationId) != null

    /** Whether [key] is the one pinned for this chat. Nothing pinned, nothing to vouch for it. */
    fun matchesPin(selfUserId: String, conversationId: String, key: ByteArray): Boolean {
        val pin = pins.load(selfUserId, conversationId) ?: return false
        return pin.commitment == commitmentOf(key)
    }

    /** What's wrong with a group's current key, or null. Updates the pin when the notice checks out. */
    fun checkGroupKey(check: GroupKeyCheck): String? {
        val pin = pins.load(check.selfUserId, check.conversationId)
        // Groups from before key notices have none, until a member's app publishes the first. Once this phone has
        // seen one, its absence means someone removed it.
        val notice = check.notice ?: return if (pin != null) GROUP_KEY_UNVERIFIED else null
        val key = check.key ?: return GROUP_KEY_UNOPENED
        if (notice.epoch != check.epoch.toLong()) return GROUP_KEY_UNVERIFIED
        if (pin != null && (notice.epoch < pin.epoch || (notice.epoch == pin.epoch && notice.seq < pin.seq))) return GROUP_KEY_UNVERIFIED
        val commitment = try {
            crypto.keyCommitment(key)
        } catch (_: CryptoException) {
            return GROUP_KEY_UNVERIFIED
        }
        val commitmentB64 = Base64Codec.encode(commitment)
        val issuer = notice.issuerId.lowercase()
        val members = notice.members.map { it.lowercase() }
        // Two notices never share a sequence number, so this one has to be the notice already seen.
        if (pin != null && notice.epoch == pin.epoch && notice.seq == pin.seq && members.toSet() != pin.members.toSet()) return GROUP_KEY_UNVERIFIED
        // The same epoch has to mean the same key.
        if (pin != null && notice.epoch == pin.epoch && pin.commitment != commitmentB64) return GROUP_KEY_UNVERIFIED
        // Someone outside the group can't start a new epoch. An unchanged key proves the issuer already holds it.
        if (pin != null && notice.epoch > pin.epoch && issuer !in pin.members) return GROUP_KEY_UNVERIFIED
        if (issuer !in members || check.selfUserId.lowercase() !in members) return GROUP_KEY_UNVERIFIED
        if (!noticeSignatureOk(check.conversationId, notice, key, check.signingKey)) return GROUP_KEY_UNVERIFIED
        val past = when {
            pin == null -> emptyMap()
            notice.epoch > pin.epoch -> pin.past + (pin.epoch.toString() to pin.commitment)
            else -> pin.past
        }
        pins.save(check.selfUserId, check.conversationId, KeyPin(notice.epoch, notice.seq, commitmentB64, members, past))
        return null
    }

    /**
     * Whether a member who held [key] signed this notice. The signature covers a commitment to the key that's never
     * sent, so nobody without the key can make one that passes, even with a signing key of their own.
     */
    fun noticeSignatureOk(conversationId: String, notice: KeyNoticeDto, key: ByteArray, signingKey: (String) -> ByteArray?): Boolean {
        val issuer = notice.issuerId.lowercase()
        val issuerKey = signingKey(issuer) ?: return false
        return try {
            val input = E2eCrypto.keyNoticeInput(conversationId, issuer, notice.epoch, notice.seq, notice.members.map { it.lowercase() }, crypto.keyCommitment(key))
            crypto.verifyMessageSignature(Base64Codec.decode(notice.signature), input, issuerKey)
        } catch (_: CryptoException) {
            false
        } catch (_: IllegalArgumentException) {
            false // Not base64.
        }
    }

    /** False for a key that isn't the one this phone verified at [epoch]. An epoch it never verified can't be checked. */
    fun matchesVerifiedEpoch(selfUserId: String, conversationId: String, epoch: Int, key: ByteArray): Boolean {
        val pin = pins.load(selfUserId, conversationId)
        val verified = if (pin?.epoch == epoch.toLong()) pin.commitment else pin?.past?.get(epoch.toString())
        return verified == null || verified == commitmentOf(key)
    }

    /**
     * Who a new notice names. The server's list isn't taken on trust: anyone it added that no notice named would
     * otherwise be confirmed by someone else's routine change. So it's the last verified list, or [latest] for someone
     * joining who has none of their own, less whoever the server no longer lists, plus [added].
     */
    fun membersToSign(selfUserId: String, conversationId: String, listed: List<String>, added: List<String>, latest: KeyNoticeDto? = null): List<String> {
        val current = listed.map { it.lowercase() }
        val verified = pins.load(selfUserId, conversationId)?.members?.takeIf { it.isNotEmpty() } ?: latest?.members?.map { it.lowercase() }
        val kept = verified?.filter { it in current } ?: current
        return (kept + added.map { it.lowercase() }).distinct()
    }

    /** People the server lists who aren't in the last verified notice. A rotation mustn't seal the key to them. */
    fun unconfirmedMembers(selfUserId: String, conversationId: String, participants: List<String>): List<String> {
        val pin = pins.load(selfUserId, conversationId) ?: return emptyList()
        if (pin.members.isEmpty()) return emptyList()
        return participants.filter { it.lowercase() !in pin.members }
    }

    /** The sequence number for this phone's next notice at [epoch]: past what it pinned and past [latest], the server's. */
    fun nextSeq(selfUserId: String, conversationId: String, epoch: Long, latest: KeyNoticeDto?): Long {
        val pinned = pins.load(selfUserId, conversationId)?.takeIf { it.epoch == epoch }?.seq ?: 0
        val published = latest?.takeIf { it.epoch == epoch }?.seq ?: 0
        return maxOf(pinned, published) + 1
    }

    private fun commitmentOf(key: ByteArray): String? = try {
        Base64Codec.encode(crypto.keyCommitment(key))
    } catch (_: CryptoException) {
        null
    }

    companion object {
        const val DIRECT_KEY_CHANGED = "This chat's key was changed, which should never happen. Nothing can be sent."
        const val DIRECT_KEY_UNOPENED = "This chat's key couldn't be opened."
        const val GROUP_KEY_UNVERIFIED = "This group's key couldn't be verified. Nothing can be sent."
        const val GROUP_KEY_UNOPENED = "This group's key couldn't be opened."
        const val UNCONFIRMED_MEMBER = "This group lists someone no member confirmed, so its key wasn't changed."
        const val CONTACT_KEY_CHANGED = "Someone's safety number changed, so nothing was sent to them. Check it in their chat first."
    }
}

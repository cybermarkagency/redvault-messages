package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

// Group invite links: a port of the web's lib/groupInviteCrypto.ts, byte for
// byte, so a link made on any of the apps opens on the others.
//
// A link is <site>/?invite=<id>#<secret>. The 32-byte secret lives only in
// the URL fragment, which never reaches any server. From it come two keys
// that can't be worked out from each other:
//   - an encryption key, under which the group's name and (when joins needn't
//     be approved) the group key are stored on the server;
//   - an auth token, shown to the server to prove the link was really opened.
// The server keeps only a hash of the token, so it can read neither.
// Members keep the secret itself encrypted with the group key, so any of
// them can show and share the link again.
//
// Compatibility with the web is pinned by test/.../GroupInviteCryptoInteropTest.kt,
// whose vectors the web's own code produced.

/** The link's two keys: [key] encrypts what the server stores, [auth] (standard base64) proves the link was opened. */
class InviteKeys(val key: ByteArray, val auth: String)

/** What the server stores for a new (or reset) link. Everything is ciphertext except the auth token, of which it keeps a hash. */
class InviteLinkMaterial(
    val auth: String,
    val secretSealed: AeadCiphertext,
    val preview: AeadCiphertext,
    /** The group key under the link's key; null when an admin approves each join. */
    val wrappedKey: AeadCiphertext?,
)

class NewInviteLink(val secret: ByteArray, val material: InviteLinkMaterial)

/** What the link shows someone about to join: the group's name and, when no approval is needed, its key. */
class OpenedInvite(val name: String, val groupKey: ByteArray?)

class GroupInviteCrypto(private val sodium: LazySodium, private val e2e: E2eCrypto) {

    fun inviteKeys(secret: ByteArray): InviteKeys =
        InviteKeys(key = hash(KEY_CONTEXT, secret), auth = Base64Codec.encode(hash(AUTH_CONTEXT, secret)))

    /** The group key under the link's key: lets people join without an admin approving. */
    fun wrapGroupKeyForLink(groupKey: ByteArray, secret: ByteArray): AeadCiphertext = e2e.aeadEncrypt(groupKey, inviteKeys(secret).key)

    /** A new link for a group: a fresh secret, and what the server stores for it. */
    fun newLink(groupKey: ByteArray, groupName: String, approval: Boolean): NewInviteLink {
        val secret = e2e.randomBytes(GroupInviteLink.SECRET_BYTES)
        val keys = inviteKeys(secret)
        // The web's JSON.stringify({ v: 1, name }): the same keys in the same order.
        val preview = buildJsonObject {
            put("v", 1)
            put("name", groupName)
        }.toString().toByteArray(Charsets.UTF_8)
        return NewInviteLink(
            secret = secret,
            material = InviteLinkMaterial(
                auth = keys.auth,
                secretSealed = e2e.aeadEncrypt(secret, groupKey),
                preview = e2e.aeadEncrypt(preview, keys.key),
                wrappedKey = if (approval) null else e2e.aeadEncrypt(groupKey, keys.key),
            ),
        )
    }

    /** A member's copy of the group's link secret, unlocked with the group key; null when it doesn't open. */
    fun openLinkSecret(secretCiphertext: ByteArray, secretNonce: ByteArray, groupKey: ByteArray): ByteArray? =
        try {
            e2e.aeadDecrypt(secretCiphertext, secretNonce, groupKey).takeIf { it.size == GroupInviteLink.SECRET_BYTES }
        } catch (e: CryptoException) {
            null
        }

    /** Opens a link's preview (and the group key, when it carries one). Throws [CryptoException] on a wrong secret. */
    fun openPreview(preview: AeadCiphertext, wrappedKey: AeadCiphertext?, secret: ByteArray): OpenedInvite {
        val key = inviteKeys(secret).key
        val plain = e2e.aeadDecrypt(preview.ciphertext, preview.nonce, key).toString(Charsets.UTF_8)
        val parsed = try {
            Json.parseToJsonElement(plain).jsonObject
        } catch (e: Exception) {
            throw CryptoException("The invite's preview isn't readable")
        }
        val groupKey = wrappedKey?.let { e2e.aeadDecrypt(it.ciphertext, it.nonce, key) }
        return OpenedInvite(name = nameOf(parsed), groupKey = groupKey)
    }

    private fun hash(context: String, secret: ByteArray): ByteArray {
        val message = context.toByteArray(Charsets.UTF_8)
        val out = ByteArray(HASH_BYTES)
        if (!sodium.cryptoGenericHash(out, out.size, message, message.size.toLong(), secret, secret.size)) throw CryptoException("Hashing failed")
        return out
    }

    private fun nameOf(preview: JsonObject): String {
        val name = preview["name"] as? JsonPrimitive
        return if (name != null && name.isString) name.content else "Group"
    }

    private companion object {
        const val HASH_BYTES = 32
        const val KEY_CONTEXT = "redvault-group-invite-v1:key"
        const val AUTH_CONTEXT = "redvault-group-invite-v1:auth"
    }
}

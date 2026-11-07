package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium
import com.goterl.lazysodium.interfaces.PwHash
import com.sun.jna.NativeLong
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put
import java.security.MessageDigest
import java.text.Normalizer
import java.util.Base64

// Public share links for Files (docs/public-share-links.md): the same keys,
// envelopes and sealed metadata as the web app (ui/src/lib/shareCrypto.ts)
// and the desktop (desktop/core/src/share_crypto.cpp), pinned to them by
// desktop/tests/vectors/share_vectors.json (ShareCryptoInteropTest).
//
// A link's 32-byte secret lives only in the URL fragment. From it (and an
// Argon2id hash of the password, when the link has one) come two tokens the
// server keeps only SHA-256 digests of (lookup, access) and a wrap key that
// never leaves the phone. The wrap key opens the envelope, which holds the
// link's share key; the share key opens each shared item's metadata.

class SharePasswordParams(val salt: ByteArray, val ops: Long = ShareCrypto.DEFAULT_OPS, val mem: Long = ShareCrypto.DEFAULT_MEM)

class ShareKeys(val lookup: ByteArray, val access: ByteArray, val wrap: ByteArray)

class ShareSealed(val ciphertext: ByteArray, val nonce: ByteArray)

class ShareEnvelope(val key: ByteArray, val upload: ByteArray?, val root: String)

/** The owner's own copy of a link's secrets, sealed under their Files key. */
data class ShareOwnerBox(val secret: String, val key: String, val root: String, val label: String, val created: Long)

class ShareKeyPair(val publicKey: ByteArray, val privateKey: ByteArray)

class ShareCrypto(private val sodium: LazySodium) {

    private val json = Json { ignoreUnknownKeys = true }

    fun newLinkSecret(): ByteArray = sodium.randomBytesBuf(32)
    fun newShareKey(): ByteArray = sodium.randomBytesBuf(32)
    fun newSalt(): ByteArray = sodium.randomBytesBuf(16)
    fun newPasswordParams(): SharePasswordParams = SharePasswordParams(newSalt())

    private fun kdf(key: ByteArray, id: Long): ByteArray {
        if (key.size != 32) throw CryptoException("Wrong key size for a share key")
        val out = ByteArray(32)
        if (sodium.cryptoKdfDeriveFromKey(out, out.size, id, CONTEXT, key) != 0) throw CryptoException("Key derivation failed")
        return out
    }

    fun lookupToken(secret: ByteArray): ByteArray {
        if (secret.size != 32) throw CryptoException("This link is incomplete")
        return kdf(secret, LOOKUP)
    }

    /**
     * The link's keys. Passwords are NFC-normalised UTF-8 on every platform.
     * Slow with a password (Argon2id): call off the main thread.
     */
    fun deriveKeys(secret: ByteArray, password: String?, params: SharePasswordParams?): ShareKeys {
        val lookup = lookupToken(secret)
        val root: ByteArray
        if (params != null) {
            if (password.isNullOrEmpty()) throw CryptoException("This link needs a password")
            if (params.salt.size != 16 || params.ops !in MIN_OPS..MAX_OPS || params.mem !in MIN_MEM..MAX_MEM) {
                throw CryptoException("This link's password settings aren't supported")
            }
            val passwordBytes = Normalizer.normalize(password, Normalizer.Form.NFC).toByteArray(Charsets.UTF_8)
            val stretched = ByteArray(32)
            try {
                val ok = sodium.cryptoPwHash(
                    stretched, stretched.size, passwordBytes, passwordBytes.size, params.salt, params.ops, NativeLong(params.mem),
                    PwHash.Alg.PWHASH_ALG_ARGON2ID13,
                )
                if (!ok) throw CryptoException("Out of memory checking the password")
                val passwordKey = kdf(secret, PASSWORD_KEY)
                val message = PASSWORD_LABEL.toByteArray(Charsets.UTF_8) + stretched
                root = ByteArray(32)
                if (!sodium.cryptoGenericHash(root, root.size, message, message.size.toLong(), passwordKey, passwordKey.size)) {
                    throw CryptoException("Key derivation failed")
                }
                message.fill(0)
                passwordKey.fill(0)
            } finally {
                stretched.fill(0)
                passwordBytes.fill(0)
            }
        } else {
            root = kdf(secret, ROOT)
        }
        try {
            return ShareKeys(lookup, kdf(root, ACCESS), kdf(root, WRAP))
        } finally {
            root.fill(0)
        }
    }

    /** What the server keeps for a token: base64 of its SHA-256. */
    fun verifierOf(token: ByteArray): String = Base64Codec.encode(MessageDigest.getInstance("SHA-256").digest(token))

    /** JSON padded with spaces to the next multiple of [bucket] bytes (at least one space). */
    fun padJson(value: JsonElement, bucket: Int): ByteArray {
        val bytes = json.encodeToString(JsonElement.serializer(), value).toByteArray(Charsets.UTF_8)
        val out = ByteArray((bytes.size / bucket + 1) * bucket) { 0x20 }
        bytes.copyInto(out)
        return out
    }

    private fun seal(key: ByteArray, ad: String, plaintext: ByteArray): ShareSealed {
        if (key.size != 32) throw CryptoException("Wrong key size")
        val nonce = sodium.randomBytesBuf(24)
        val adBytes = ad.toByteArray(Charsets.UTF_8)
        val out = ByteArray(plaintext.size + 16)
        val len = LongArray(1)
        if (!sodium.cryptoAeadXChaCha20Poly1305IetfEncrypt(out, len, plaintext, plaintext.size.toLong(), adBytes, adBytes.size.toLong(), null, nonce, key)) {
            throw CryptoException("Couldn't seal")
        }
        return ShareSealed(out.copyOf(len[0].toInt()), nonce)
    }

    private fun open(key: ByteArray, ad: String, ciphertext: ByteArray, nonce: ByteArray): ByteArray? {
        if (key.size != 32 || nonce.size != 24 || ciphertext.size < 16) return null
        val adBytes = ad.toByteArray(Charsets.UTF_8)
        val out = ByteArray(ciphertext.size - 16)
        val len = LongArray(1)
        if (!sodium.cryptoAeadXChaCha20Poly1305IetfDecrypt(out, len, null, ciphertext, ciphertext.size.toLong(), adBytes, adBytes.size.toLong(), nonce, key)) {
            return null
        }
        return out.copyOf(len[0].toInt())
    }

    private fun parsePadded(bytes: ByteArray): JsonObject? = runCatching {
        json.parseToJsonElement(String(bytes, Charsets.UTF_8).trimEnd(' ')) as? JsonObject
    }.getOrNull()

    fun sealEnvelope(wrap: ByteArray, envelope: ShareEnvelope): ShareSealed {
        val j = buildJsonObject {
            put("v", 1)
            put("key", Base64Codec.encode(envelope.key))
            put("upload", envelope.upload?.let { JsonPrimitive(Base64Codec.encode(it)) } ?: JsonNull)
            put("root", envelope.root)
        }
        return seal(wrap, ENVELOPE_AD, padJson(j, 128))
    }

    /** Null for a wrong key (a wrong password) or anything altered. */
    fun openEnvelope(wrap: ByteArray, ciphertext: ByteArray, nonce: ByteArray): ShareEnvelope? {
        val j = open(wrap, ENVELOPE_AD, ciphertext, nonce)?.let(::parsePadded) ?: return null
        if (j.int("v") != 1L) return null
        val key = j.str("key")?.let { runCatching { Base64Codec.decode(it) }.getOrNull() } ?: return null
        val root = j.str("root") ?: return null
        val upload = j.str("upload")?.let { runCatching { Base64Codec.decode(it) }.getOrNull() ?: return null }
        if (key.size != 32 || (upload != null && upload.size != 32)) return null
        return ShareEnvelope(key, upload, root)
    }

    fun sealItem(shareKey: ByteArray, itemId: String, meta: JsonObject): ShareSealed = seal(shareKey, ITEM_AD_PREFIX + itemId, padJson(meta, 128))

    /** The item's metadata, checked field by field; null if it doesn't open under this key and id, or is malformed. */
    fun openItem(shareKey: ByteArray, itemId: String, ciphertext: ByteArray, nonce: ByteArray): JsonObject? {
        val m = open(shareKey, ITEM_AD_PREFIX + itemId, ciphertext, nonce)?.let(::parsePadded) ?: return null
        if (m.int("v") != 1L) return null
        val name = m.str("name")?.takeIf { it.length <= 1024 } ?: return null
        val parent = m.str("parent")?.takeIf { it.length <= 64 } ?: return null
        return when (m.str("kind")) {
            "folder" -> folderMeta(name, parent)
            "file" -> {
                val mime = m.str("mime")?.takeIf { it.length <= 255 } ?: return null
                val size = m.count("size") ?: return null
                val modified = m.count("modified") ?: return null
                val key = m.str("key")?.takeIf { it.length <= 64 } ?: return null
                val header = m.str("header")?.takeIf { it.length <= 64 } ?: return null
                val chunk = m.count("chunk") ?: return null
                val sha = m.str("sha")?.takeIf { HEX64.matches(it) } ?: return null
                fileMeta(name, parent, mime, size, modified, key, header, chunk, sha)
            }
            else -> null
        }
    }

    fun sealOwnerBox(driveKey: ByteArray, box: ShareOwnerBox): ShareSealed {
        val j = buildJsonObject {
            put("v", 1)
            put("secret", box.secret)
            put("key", box.key)
            put("root", box.root)
            put("label", box.label)
            put("created", box.created)
        }
        return seal(driveKey, OWNER_AD, padJson(j, 128))
    }

    fun openOwnerBox(driveKey: ByteArray, ciphertext: ByteArray, nonce: ByteArray): ShareOwnerBox? {
        val b = open(driveKey, OWNER_AD, ciphertext, nonce)?.let(::parsePadded) ?: return null
        if (b.int("v") != 1L) return null
        return ShareOwnerBox(
            secret = b.str("secret")?.takeIf { it.length <= 64 } ?: return null,
            key = b.str("key")?.takeIf { it.length <= 64 } ?: return null,
            root = b.str("root")?.takeIf { it.length <= 64 } ?: return null,
            label = b.str("label")?.takeIf { it.length <= 1024 } ?: return null,
            created = b.count("created") ?: return null,
        )
    }

    /** The owner's upload key pair for one link: derived from the Files key and the link's salt, never stored. */
    fun uploadKeyPair(driveKey: ByteArray, salt: ByteArray): ShareKeyPair {
        if (salt.size != 16) throw CryptoException("Wrong upload key salt")
        val message = UPLOAD_LABEL.toByteArray(Charsets.UTF_8) + salt
        val seed = ByteArray(32)
        if (!sodium.cryptoGenericHash(seed, seed.size, message, message.size.toLong(), driveKey, driveKey.size)) throw CryptoException("Key derivation failed")
        val publicKey = ByteArray(32)
        val privateKey = ByteArray(32)
        try {
            if (!sodium.cryptoBoxSeedKeypair(publicKey, privateKey, seed)) throw CryptoException("Key derivation failed")
        } finally {
            seed.fill(0)
        }
        return ShareKeyPair(publicKey, privateKey)
    }

    /** What an anonymous uploader sealed to the owner, checked; or null. */
    fun openContribution(pair: ShareKeyPair, sealed: ByteArray): JsonObject? {
        if (sealed.size < 48) return null
        val out = ByteArray(sealed.size - 48)
        if (!sodium.cryptoBoxSealOpen(out, sealed, sealed.size.toLong(), pair.publicKey, pair.privateKey)) return null
        val m = parsePadded(out) ?: return null
        if (m.int("v") != 1L) return null
        val ok = m.str("name")?.let { it.length <= 1024 } == true && m.str("mime")?.let { it.length <= 255 } == true &&
            m.count("size") != null && m.count("modified") != null && m.count("sent") != null &&
            m.str("key")?.let { it.length <= 64 } == true && m.str("header")?.let { it.length <= 64 } == true &&
            m.count("chunk") != null && m.str("sha")?.let { HEX64.matches(it) } == true
        return if (ok) m else null
    }

    /** https://host/redvaultmessages/share/?id=<publicId>#<secret, base64url>. */
    fun shareUrl(webAppBase: String, publicId: String, secret: ByteArray): String {
        val base = if (webAppBase.endsWith("/")) webAppBase else "$webAppBase/"
        return "${base}share/?id=$publicId#${toFragment(secret)}"
    }

    fun toFragment(secret: ByteArray): String = Base64.getUrlEncoder().withoutPadding().encodeToString(secret)

    fun fromFragment(fragment: String): ByteArray? =
        runCatching { Base64.getUrlDecoder().decode(fragment.removePrefix("#")) }.getOrNull()?.takeIf { it.size == 32 }

    companion object {
        const val DEFAULT_OPS = 3L
        const val DEFAULT_MEM = 64L * 1024 * 1024
        private const val MIN_OPS = 2L
        private const val MAX_OPS = 10L
        private const val MIN_MEM = 64L * 1024 * 1024
        private const val MAX_MEM = 256L * 1024 * 1024
        private val CONTEXT = "rvshare1".toByteArray(Charsets.US_ASCII)
        private const val LOOKUP = 1L
        private const val ROOT = 2L
        private const val PASSWORD_KEY = 3L
        private const val ACCESS = 4L
        private const val WRAP = 5L
        private const val ENVELOPE_AD = "rvshare-envelope-v1"
        private const val ITEM_AD_PREFIX = "rvshare-item-v1|"
        private const val OWNER_AD = "rvshare-owner-v1"
        private const val UPLOAD_LABEL = "redvault-share-upload-v1"
        private const val PASSWORD_LABEL = "redvault-share-password-v1"
        private val HEX64 = Regex("^[0-9a-f]{64}$")

        fun folderMeta(name: String, parent: String): JsonObject = buildJsonObject {
            put("v", 1)
            put("kind", "folder")
            put("name", name)
            put("parent", parent)
        }

        fun fileMeta(name: String, parent: String, mime: String, size: Long, modified: Long, key: String, header: String, chunk: Long, sha: String): JsonObject =
            buildJsonObject {
                put("v", 1)
                put("kind", "file")
                put("name", name)
                put("parent", parent)
                put("mime", mime)
                put("size", size)
                put("modified", modified)
                put("key", key)
                put("header", header)
                put("chunk", chunk)
                put("sha", sha)
            }

        private fun JsonObject.str(key: String): String? = (this[key] as? JsonPrimitive)?.takeIf { it.isString }?.contentOrNull

        /** A whole number from 0 to 2^53 - 1. */
        private fun JsonObject.count(key: String): Long? {
            val p = this[key] as? JsonPrimitive ?: return null
            if (p.isString) return null
            val v = p.longOrNull ?: p.content.toDoubleOrNull()?.takeIf { it == Math.floor(it) }?.toLong() ?: return null
            return v.takeIf { it in 0..9007199254740991L }
        }

        private fun JsonObject.int(key: String): Long? = (this[key] as? JsonPrimitive)?.takeIf { !it.isString }?.longOrNull
    }
}

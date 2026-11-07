package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.DriveCrypto
import com.redvaultsystems.redvaultmessage.crypto.PasswordKdf
import com.redvaultsystems.redvaultmessage.crypto.WrappedKey
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put

/** One of the person's own file-locking keys (never the key itself: see [FileKeyRepository.open]). */
data class FileKey(
    val id: String,
    val name: String,
    /** "7F3A-19C2-E04B-D5A8": the same on every device, safe to show. */
    val fingerprint: String,
    val passwordProtected: Boolean,
    val imported: Boolean,
    val createdMs: Long,
)

/**
 * The person's own keys for locking files, kept as password-vault items of
 * type "filekey" (a port of the desktop app's FileKeyRepository, the same
 * item format). A key can also have a password of its own: then the item
 * holds it wrapped under Argon2id(password), and only someone with both the
 * account and that password can use it.
 */
class FileKeyRepository(private val vault: VaultRepository, private val drive: DriveCrypto, private val locks: FileLocks) {

    suspend fun list(): List<FileKey> = vault.list().filter { !it.incoming && isFileKeyItem(it.data) }.map(::fromEntry)

    /** Makes a new random key. An empty password leaves it protected by the account (the vault) alone. */
    suspend fun create(name: String, password: String): FileKey {
        val entry = VaultEntry(data = buildJsonObject { put("type", TYPE); put("title", cleanName(name)); put("imported", false) })
        return save(entry, drive.newFileLockKey(), password)
    }

    /** Brings in a key made outside the app. Refuses anything that isn't 256 bits, and a key that's already here. */
    suspend fun import(name: String, keyText: String, password: String): FileKey {
        val secret = drive.parseFileLockKey(keyText) ?: throw UserFacingException("That isn't a valid key. Paste 64 hex characters or the base64 of a 256-bit key.")
        val fingerprint = drive.fileLockKeyFingerprint(secret)
        list().firstOrNull { it.fingerprint == fingerprint }?.let { throw UserFacingException("You already have this key: “${it.name}”.") }
        val entry = VaultEntry(data = buildJsonObject { put("type", TYPE); put("title", cleanName(name)); put("imported", true) })
        return save(entry, secret, password)
    }

    /** The key itself, to lock or unlock files with. Throws [UserFacingException] on a wrong password. Slow with one. */
    suspend fun open(key: FileKey, password: String): ByteArray = withContext(Dispatchers.Default) {
        val data = entryFor(key).data
        val fingerprint = data.stringOr("fingerprint", "")
        try {
            val secret = if (data.containsKey("lock")) {
                if (password.isEmpty()) throw UserFacingException("Enter this key's password.")
                val lock = data["lock"]!!.jsonObject
                val kdf = PasswordKdf(Base64Codec.decode(lock.stringOr("salt", "")), lock["ops"]!!.jsonPrimitive.long, lock["mem"]!!.jsonPrimitive.long)
                val wrappingKey = drive.passwordKey(password, kdf)
                try {
                    drive.unwrapKey(WrappedKey(Base64Codec.decode(lock.stringOr("ct", "")), Base64Codec.decode(lock.stringOr("nonce", ""))), wrappingKey, context(fingerprint))
                } catch (e: Exception) {
                    throw UserFacingException("Wrong password.")
                } finally {
                    wrappingKey.fill(0)
                }
            } else {
                Base64Codec.decode(data.stringOr("key", ""))
            }
            if (secret.size != DriveCrypto.FILE_LOCK_KEY_BYTES || drive.fileLockKeyFingerprint(secret) != fingerprint) {
                throw UserFacingException("This key is damaged, so it can't be used.")
            }
            secret
        } catch (e: UserFacingException) {
            throw e
        } catch (e: Exception) {
            throw UserFacingException("This key is damaged, so it can't be used.")
        }
    }

    /** Sets, changes or (empty [newPassword]) removes the key's own password. Files locked with it stay as they are. */
    suspend fun setPassword(key: FileKey, currentPassword: String, newPassword: String): FileKey {
        val secret = open(key, currentPassword)
        try {
            return save(entryFor(key), secret, newPassword)
        } finally {
            secret.fill(0)
        }
    }

    suspend fun rename(key: FileKey, name: String): FileKey {
        val entry = entryFor(key)
        return fromEntry(vault.save(entry.copy(data = JsonObject(entry.data + ("title" to JsonPrimitive(cleanName(name)))))))
    }

    /** The key as 64 hex characters, for keeping a copy outside the app. */
    suspend fun exportHex(key: FileKey, password: String): String {
        val secret = open(key, password)
        try {
            return drive.fileLockKeyHex(secret)
        } finally {
            secret.fill(0)
        }
    }

    /** Deletes it. Files locked with it can't be opened again unless it's imported back. */
    suspend fun remove(key: FileKey) = vault.remove(entryFor(key))

    private suspend fun entryFor(key: FileKey): VaultEntry =
        vault.list().firstOrNull { it.id == key.id && !it.incoming && isFileKeyItem(it.data) } ?: throw UserFacingException("That key isn't there anymore.")

    private suspend fun save(entry: VaultEntry, secret: ByteArray, password: String): FileKey {
        if (password.isNotEmpty()) locks.passwordProblem(password)?.let { throw UserFacingException(it) }
        val fingerprint = drive.fileLockKeyFingerprint(secret)
        val data = entry.data.toMutableMap()
        data["fingerprint"] = JsonPrimitive(fingerprint)
        data.remove("key")
        data.remove("lock")
        if (password.isEmpty()) {
            data["key"] = JsonPrimitive(Base64Codec.encode(secret))
        } else {
            data["lock"] = withContext(Dispatchers.Default) {
                val kdf = drive.newPasswordKdf()
                val wrappingKey = drive.passwordKey(password, kdf)
                try {
                    val wrapped = drive.wrapKey(secret, wrappingKey, context(fingerprint))
                    buildJsonObject {
                        put("v", 1)
                        put("salt", Base64Codec.encode(kdf.salt))
                        put("ops", kdf.opsLimit)
                        put("mem", kdf.memLimit)
                        put("ct", Base64Codec.encode(wrapped.ciphertext))
                        put("nonce", Base64Codec.encode(wrapped.nonce))
                    }
                } finally {
                    wrappingKey.fill(0)
                }
            }
        }
        return fromEntry(vault.save(entry.copy(data = JsonObject(data))))
    }

    companion object {
        private const val TYPE = "filekey"

        /** Whether a vault item is a file key (the Passwords list leaves these out). */
        fun isFileKeyItem(data: JsonObject): Boolean = data.stringOr("type", "") == TYPE

        private fun context(fingerprint: String) = "redvault-file-key-v1|$fingerprint"

        private fun cleanName(name: String): String = name.take(80).trimEnd(' ', '\t').ifEmpty { "File key" }

        private fun fromEntry(entry: VaultEntry) = FileKey(
            id = entry.id,
            name = entry.data.stringOr("title", "File key"),
            fingerprint = entry.data.stringOr("fingerprint", ""),
            passwordProtected = entry.data.containsKey("lock"),
            imported = entry.data.boolOr("imported", false),
            createdMs = entry.data.longOr("created", 0),
        )
    }
}

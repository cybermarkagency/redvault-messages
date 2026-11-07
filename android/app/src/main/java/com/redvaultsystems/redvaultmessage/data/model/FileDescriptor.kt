package com.redvaultsystems.redvaultmessage.data.model

import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import java.text.Normalizer

// Port of ui/src/lib/fileMessage.ts: the descriptor that rides inside an
// ordinary E2EE message, so the file key, real filename, type and size are
// only ever readable by the chat's participants. Everything parsed from a
// received descriptor is untrusted input from the other participant.

@Serializable
data class FileEncryptionInfo(val algorithm: String, val version: Int, val header: String, val chunkSize: Int)

@Serializable
data class FileDescriptor(
    val type: String,
    val version: Int,
    val objectId: String,
    /** base64: the 256-bit file key. */
    val fileKey: String,
    val filename: String,
    val mimeType: String,
    val originalSize: Long,
    val encryptedSize: Long,
    /** hex SHA-256 of the ciphertext; the recipient checks it before trusting anything. */
    val ciphertextSha256: String,
    val encryption: FileEncryptionInfo,
)

object FileMessage {
    /** A NUL can't be typed into the composer, so a text message can't impersonate a file. */
    const val PREFIX = "\u0000rv-file:"

    private val json = Json { encodeDefaults = true }
    private val UUID_RE = Regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", RegexOption.IGNORE_CASE)
    private val B64_RE = Regex("^[A-Za-z0-9+/]+={0,2}$")
    private val HEX64_RE = Regex("^[0-9a-f]{64}$")
    private val MIME_RE = Regex("^[a-z0-9][a-z0-9!#$&^_.+-]{0,63}/[a-z0-9][a-z0-9!#$&^_.+-]{0,127}$", RegexOption.IGNORE_CASE)
    // Device names, with or without an extension. Superscript digits count (COM¹), and a space before the dot doesn't help.
    private val WINDOWS_RESERVED = Regex("^(con|prn|aux|nul|conin\\$|conout\\$|com[0-9¹²³]|lpt[0-9¹²³])(\\s*\\..*)?$", RegexOption.IGNORE_CASE)
    // Control characters, bidi overrides/isolates, zero-width characters, BOM.
    private val HIDDEN_CHARS = Regex("[\\u0000-\\u001f\\u007f-\\u009f\\u200b-\\u200f\\u202a-\\u202e\\u2060-\\u2069\\ufeff]")

    fun isFileMessage(plaintext: String): Boolean = plaintext.startsWith(PREFIX)

    fun serialize(descriptor: FileDescriptor): String = PREFIX + json.encodeToString(FileDescriptor.serializer(), descriptor)

    /** Parses and strictly validates a received descriptor; null for anything malformed. */
    fun parse(plaintext: String): FileDescriptor? {
        if (!isFileMessage(plaintext)) return null
        val d: JsonObject = runCatching { json.parseToJsonElement(plaintext.removePrefix(PREFIX)).jsonObject }.getOrNull() ?: return null
        fun str(o: JsonObject, k: String) = runCatching { o[k]?.jsonPrimitive?.takeIf { it.isString }?.contentOrNull }.getOrNull()
        fun int(o: JsonObject, k: String) = runCatching { o[k]?.jsonPrimitive?.takeIf { !it.isString }?.intOrNull }.getOrNull()
        fun size(o: JsonObject, k: String) = runCatching { o[k]?.jsonPrimitive?.takeIf { !it.isString }?.longOrNull }.getOrNull()
            ?.takeIf { it in 0..MAX_SAFE_INTEGER }
        val enc = runCatching { d["encryption"]?.jsonObject }.getOrNull() ?: return null

        val objectId = str(d, "objectId")?.takeIf(UUID_RE::matches) ?: return null
        val fileKey = str(d, "fileKey")?.takeIf(B64_RE::matches) ?: return null
        val filename = str(d, "filename")?.takeIf { it.length <= 1024 } ?: return null
        val mimeType = str(d, "mimeType") ?: return null
        val originalSize = size(d, "originalSize") ?: return null
        val encryptedSize = size(d, "encryptedSize") ?: return null
        val sha = str(d, "ciphertextSha256")?.takeIf(HEX64_RE::matches) ?: return null
        val header = str(enc, "header")?.takeIf(B64_RE::matches) ?: return null
        if (str(d, "type") != "file" || int(d, "version") != FileCrypto.FORMAT_VERSION) return null
        if (str(enc, "algorithm") != FileCrypto.ALGORITHM || int(enc, "version") != FileCrypto.FORMAT_VERSION) return null
        if (int(enc, "chunkSize") != FileCrypto.CHUNK_BYTES) return null

        return FileDescriptor(
            type = "file",
            version = FileCrypto.FORMAT_VERSION,
            objectId = objectId.lowercase(),
            fileKey = fileKey,
            filename = sanitizeFilename(filename),
            mimeType = if (MIME_RE.matches(mimeType)) mimeType.lowercase() else "application/octet-stream",
            originalSize = originalSize,
            encryptedSize = encryptedSize,
            ciphertextSha256 = sha,
            encryption = FileEncryptionInfo(FileCrypto.ALGORITHM, FileCrypto.FORMAT_VERSION, header, FileCrypto.CHUNK_BYTES),
        )
    }

    /**
     * Makes a sender-supplied filename safe to show and to save under: keeps
     * only the last path component (no traversal), strips control, bidi and
     * zero-width characters (so "invoice‮gpj.exe" can't render as
     * "invoiceexe.jpg"), leading dots and Windows-reserved names, and caps
     * the length while keeping the extension.
     */
    fun sanitizeFilename(name: String): String {
        var clean = Normalizer.normalize(name, Normalizer.Form.NFC)
            .split('/', '\\').last()
            .replace(HIDDEN_CHARS, "")
            .replace(Regex("[<>:\"|?*]"), "_")
            .replace(Regex("\\s+"), " ")
            .trim()
            .replace(Regex("^\\.+"), "")
            .replace(Regex("[. ]+$"), "")
        if (WINDOWS_RESERVED.matches(clean)) clean = "_$clean"
        if (clean.length > MAX_FILENAME) {
            val dot = clean.lastIndexOf('.')
            val ext = if (dot > 0 && clean.length - dot <= 16) clean.substring(dot) else ""
            // Cutting can leave a trailing space or dot, which Windows drops when the file reaches a PC.
            clean = (clean.substring(0, MAX_FILENAME - ext.length) + ext).trimEnd('.', ' ')
        }
        return clean.ifEmpty { "file" }
    }

    fun formatBytes(bytes: Long): String {
        if (bytes < 1024) return "$bytes B"
        val units = listOf("KB", "MB", "GB")
        var value = bytes / 1024.0
        var i = 0
        while (value >= 1024 && i < units.size - 1) {
            value /= 1024
            i++
        }
        return if (value < 10) "%.1f %s".format(value, units[i]) else "${Math.round(value)} ${units[i]}"
    }

    private const val MAX_FILENAME = 150
    private const val MAX_SAFE_INTEGER = 9_007_199_254_740_991L
}

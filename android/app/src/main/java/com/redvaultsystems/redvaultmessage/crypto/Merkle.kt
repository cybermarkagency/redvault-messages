package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium
import java.security.MessageDigest

/**
 * Verifiers for the transparency log: a port of ui/src/lib/merkle.ts and
 * transparencyLog.ts (RFC 9162 Merkle proofs, Ed25519-signed checkpoints),
 * so this phone checks the log itself instead of taking the server's word.
 */
object Merkle {
    fun fromHex(hex: String): ByteArray = ByteArray(hex.length / 2) { i -> hex.substring(i * 2, i * 2 + 2).toInt(16).toByte() }

    fun toHex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) }

    private fun sha256(vararg parts: ByteArray): ByteArray {
        val digest = MessageDigest.getInstance("SHA-256")
        parts.forEach(digest::update)
        return digest.digest()
    }

    fun leafHash(data: ByteArray): ByteArray = sha256(byteArrayOf(0), data)

    private fun nodeHash(left: ByteArray, right: ByteArray): ByteArray = sha256(byteArrayOf(1), left, right)

    /** RFC 9162 2.1.3.2 */
    fun verifyInclusion(leaf: ByteArray, index: Long, size: Long, proof: List<ByteArray>, root: ByteArray): Boolean {
        if (index >= size) return false
        var fn = index
        var sn = size - 1
        var r = leaf
        for (p in proof) {
            if (sn == 0L) return false
            if (fn % 2 == 1L || fn == sn) {
                r = nodeHash(p, r)
                while (fn % 2 == 0L && fn != 0L) {
                    fn /= 2
                    sn /= 2
                }
            } else {
                r = nodeHash(r, p)
            }
            fn /= 2
            sn /= 2
        }
        return sn == 0L && r.contentEquals(root)
    }

    /** RFC 9162 2.1.4.2 */
    fun verifyConsistency(from: Long, to: Long, fromRoot: ByteArray, toRoot: ByteArray, proof: List<ByteArray>): Boolean {
        if (from < 1 || from > to) return false
        if (from == to) return proof.isEmpty() && fromRoot.contentEquals(toRoot)
        val path = if (from and (from - 1) == 0L) listOf(fromRoot) + proof else proof
        if (path.isEmpty()) return false
        var fn = from - 1
        var sn = to - 1
        while (fn % 2 == 1L) {
            fn /= 2
            sn /= 2
        }
        var fr = path[0]
        var sr = path[0]
        for (c in path.drop(1)) {
            if (sn == 0L) return false
            if (fn % 2 == 1L || fn == sn) {
                fr = nodeHash(c, fr)
                sr = nodeHash(c, sr)
                while (fn % 2 == 0L && fn != 0L) {
                    fn /= 2
                    sn /= 2
                }
            } else {
                sr = nodeHash(sr, c)
            }
            fn /= 2
            sn /= 2
        }
        return sn == 0L && fr.contentEquals(fromRoot) && sr.contentEquals(toRoot)
    }

    /** Exactly what the server signs (transparencyLogService.ts's checkpointMessage). */
    fun checkpointMessage(treeSize: Long, rootHashHex: String, signedAtIso: String): String =
        "redvault-transparency-log-v1\n$treeSize\n$rootHashHex\n$signedAtIso"

    fun signatureValid(sodium: LazySodium, treeSize: Long, rootHash: String, signedAt: String, signatureB64: String, publicKeyB64: String): Boolean = try {
        val signature = Base64Codec.decode(signatureB64)
        val publicKey = Base64Codec.decode(publicKeyB64)
        val message = checkpointMessage(treeSize, rootHash, signedAt).toByteArray(Charsets.UTF_8)
        signature.size == 64 && publicKey.size == 32 && sodium.cryptoSignVerifyDetached(signature, message, message.size, publicKey)
    } catch (e: Exception) {
        false
    }

    /** A short, readable fingerprint of the log's signing key, to compare with what the operator publishes. */
    fun keyFingerprint(publicKeyB64: String): String =
        runCatching { toHex(Base64Codec.decode(publicKeyB64)).take(16).chunked(4).joinToString(" ") }.getOrDefault("")
}

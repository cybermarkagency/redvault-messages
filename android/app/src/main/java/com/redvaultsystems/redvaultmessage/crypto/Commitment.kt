// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

// Port of ui/src/lib/commitment.ts, the client half of stage-3 "Transaction
// verified". MUST match server/src/crypto/commitment.ts byte-for-byte: the
// client recomputes the commitment over the exact encrypted envelope it
// sent and compares it with the server's, rather than trusting its word.
object Commitment {
    const val PROTOCOL_VERSION = 1

    /** v1 = "v1:" + messageId + ":" + base64(ciphertext) + ":" + base64(nonce) */
    fun canonicalEnvelopeString(
        messageId: String,
        ciphertext: ByteArray,
        nonce: ByteArray,
        protocolVersion: Int = PROTOCOL_VERSION,
    ): String {
        require(protocolVersion == PROTOCOL_VERSION) { "Unsupported commitment protocol version: $protocolVersion" }
        return "v$protocolVersion:$messageId:${Base64Codec.encode(ciphertext)}:${Base64Codec.encode(nonce)}"
    }

    /** Lowercase hex SHA-256 of canonicalEnvelopeString(). */
    fun computeHex(messageId: String, ciphertext: ByteArray, nonce: ByteArray): String =
        E2eCrypto.sha256(canonicalEnvelopeString(messageId, ciphertext, nonce).toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }

    /** A commitment is a public hash, not a secret, so plain comparison is fine. */
    fun matches(localHex: String, networkHex: String): Boolean = localHex.equals(networkHex, ignoreCase = true)
}

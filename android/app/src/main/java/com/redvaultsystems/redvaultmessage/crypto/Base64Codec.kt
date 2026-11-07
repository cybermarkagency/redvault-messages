package com.redvaultsystems.redvaultmessage.crypto

import java.util.Base64

/**
 * Standard, padded base64: the network/DB boundary format the server and
 * web client use for every key, nonce and ciphertext (libsodium's
 * base64_variants.ORIGINAL).
 */
object Base64Codec {
    fun encode(bytes: ByteArray): String = Base64.getEncoder().encodeToString(bytes)

    fun decode(value: String): ByteArray = Base64.getDecoder().decode(value)
}

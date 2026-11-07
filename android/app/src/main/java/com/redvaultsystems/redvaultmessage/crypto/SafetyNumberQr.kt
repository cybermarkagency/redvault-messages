package com.redvaultsystems.redvaultmessage.crypto

/**
 * The QR code two people scan to verify each other in person. It carries
 * the 60-digit safety number, which is a hash of both public keys and is
 * the same on both phones regardless of order. So if the number scanned
 * from their screen equals the one this phone computed, both phones are
 * using the same two keys and nobody in between has swapped one.
 *
 * It holds nothing secret: the same digits are shown on screen for people
 * comparing by eye (e.g. with someone on the web app, which can't scan).
 */
object SafetyNumberQr {
    private const val PREFIX = "redvault-safety:v1:"
    private const val DIGITS = 60

    enum class Result { Match, Mismatch, NotASafetyCode }

    fun encode(safetyNumber: List<String>): String = PREFIX + safetyNumber.joinToString("")

    fun compare(scanned: String, mine: List<String>): Result {
        val trimmed = scanned.trim()
        if (!trimmed.startsWith(PREFIX)) return Result.NotASafetyCode
        val digits = trimmed.removePrefix(PREFIX)
        if (digits.length != DIGITS || !digits.all(Char::isDigit)) return Result.NotASafetyCode
        return if (digits == mine.joinToString("")) Result.Match else Result.Mismatch
    }
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.calls

import com.redvaultsystems.redvaultmessage.crypto.GroupCallCrypto
import io.livekit.android.e2ee.BaseKeyProvider
import io.livekit.android.e2ee.KeyProvider
import livekit.org.webrtc.FrameCryptorKeyDerivationAlgorithm

/**
 * LiveKit key provider for one shared room key per epoch: the Android side of
 * the web's RoomKeyProvider (ui/src/lib/groupCallCrypto.ts), so the two derive
 * and install keys the same way.
 *
 *   - The room key is 32 random bytes, used as raw key material (not a
 *     password string): AES-128-GCM key = HKDF-SHA256(key, salt
 *     "LKFrameEncryptionKey", 128 zero bytes), which is what the browser does
 *     when handed an ArrayBuffer. The same derivation runs in libwebrtc when
 *     asked for [FrameCryptorKeyDerivationAlgorithm.HKDF].
 *   - Each epoch goes in its own key slot (epoch mod 16), so frames already in
 *     flight under the previous key still decrypt while everyone switches
 *     over. Receivers pick the slot from each frame's header; what this
 *     provider decides is the slot this phone *sends* with ([sendSlot]).
 *   - Never ratchets or guesses keys (a window of 0, no failure limit).
 *
 * LiveKit's own [BaseKeyProvider] only takes keys as strings and always sends
 * with slot 0 in shared-key mode, so this wraps it: raw bytes go straight to
 * the native key provider, and [getLatestKeyIndex] says which slot to send with.
 */
class RoomKeyProvider private constructor(private val base: BaseKeyProvider) : KeyProvider by base {
    constructor() : this(
        BaseKeyProvider(
            ratchetWindowSize = 0,
            enableSharedKey = true,
            failureTolerance = -1,
            keyRingSize = GroupCallCrypto.KEYRING_SIZE,
            keyDerivationAlgorithm = FrameCryptorKeyDerivationAlgorithm.HKDF,
        ),
    )

    /** The slot new frames are encrypted under: the newest epoch's. */
    @Volatile
    var sendSlot: Int = 0
        private set

    /** Whether any key is installed yet. */
    @Volatile
    var hasKey: Boolean = false
        private set

    override fun getLatestKeyIndex(participantId: String): Int = sendSlot

    /** Frees the native key provider (and the keys in it). */
    fun dispose() {
        hasKey = false
        base.rtcKeyProvider.dispose()
    }

    /** Installs the room key for [epoch] in its slot and makes it the one to send with. Returns false if libwebrtc refused it. */
    fun installKey(key: ByteArray, epoch: Long): Boolean {
        val slot = GroupCallCrypto.keySlot(epoch)
        if (!base.rtcKeyProvider.setSharedKey(slot, key)) return false
        sendSlot = slot
        hasKey = true
        return true
    }
}

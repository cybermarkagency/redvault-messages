// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.calls

import com.redvaultsystems.redvaultmessage.data.api.IceServerDto

/**
 * TURN credentials for long one-to-one calls (docs/tor-routing-plan.md,
 * decision 5). The server's coturn REST credentials have the username
 * "<expiry, unix seconds>:<random>", and coturn checks that expiry on every
 * authenticated request, so a call that outlives its credentials loses its
 * relay within minutes. Before that, the caller fetches fresh credentials and
 * restarts ICE with them (CallManager.renewRelay); the callee answers with
 * fresh credentials of its own. The same as the web app (ui/src/lib/turnRenewal.ts).
 */
object TurnRenewal {
    private val EXPIRY = Regex("^(\\d{9,12}):")

    /** When the credentials stop working (epoch ms), or null if they don't say. */
    fun expiryMs(servers: List<IceServerDto>): Long? =
        servers.mapNotNull { EXPIRY.find(it.username)?.groupValues?.get(1)?.toLongOrNull()?.times(1000) }.minOrNull()

    /** Renew at 80% of what's left, and never sooner than a minute from now. */
    fun delayMs(expiryMs: Long, nowMs: Long = System.currentTimeMillis()): Long = maxOf(60_000L, ((expiryMs - nowMs) * 0.8).toLong())
}

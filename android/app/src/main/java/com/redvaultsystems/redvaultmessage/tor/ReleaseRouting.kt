// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.tor

import java.net.URI

/**
 * Tor by default, one app (docs/tor-by-default.md): a release build only ever talks to the service's onion address,
 * through the embedded Tor gateway. The build refuses any other API base (checkTorOnlyRelease in app/build.gradle.kts);
 * the app checks the same at start-up. Only a debug build may use another server: a local test server, for development.
 */
object ReleaseRouting {
    /** The live service's onion address (GET /api/public/routing on the website names it). */
    const val ONION_HOST = "4cydjhdc3lmhg7l6ykjont7dms2alunglmnlznlmjjwkyc67m5h325yd.onion"
    const val ONION_API_BASE = "http://$ONION_HOST/redvaultmessages/api/"

    /** Why [apiBase] can't be used by this build, or null when it can. */
    fun refusal(apiBase: String, debug: Boolean): String? {
        if (debug) return null
        val uri = runCatching { URI(apiBase) }.getOrNull()
        val host = uri?.host.orEmpty()
        return when {
            uri == null || uri.scheme != "http" || !TorController.isOnionHost(host) ->
                "This build only connects through Tor to the service's onion address."
            !isV3Onion(host) -> "That isn't a Tor onion service address."
            else -> null
        }
    }

    /** A v3 onion name: 56 base32 characters and ".onion". */
    fun isV3Onion(host: String): Boolean = Regex("^[a-z2-7]{56}\\.onion$").matches(host.lowercase())
}

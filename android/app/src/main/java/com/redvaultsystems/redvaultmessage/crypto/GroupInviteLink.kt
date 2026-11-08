// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import java.net.URLDecoder
import java.net.URLEncoder
import java.util.Base64

/**
 * Group invite link addresses, …/?invite=<id>#<secret>: the URL half of the
 * web's lib/groupInviteCrypto.ts (groupInviteUrl, parseGroupInvite,
 * inviteSecretFromFragment), with the same rules. The secret is base64url
 * without padding and sits only in the fragment.
 */
object GroupInviteLink {
    const val SECRET_BYTES = 32

    private const val PARAM = "invite"
    private val LINK_ID = Regex("^[A-Za-z0-9_-]{1,64}$")
    private val SECRET = Regex("^[A-Za-z0-9_-]{43}$")
    private val BASE64URL = Regex("^[A-Za-z0-9_-]*$")
    private val ADDRESS = Regex("https?://\\S+")

    /** A pasted (or tapped) invite link: the link's id and the secret, still base64url. */
    data class Parsed(val linkId: String, val secret: String)

    /** The shareable address: the web app's ([webAppBase], …/redvaultmessages/), the link id, and the secret in the fragment. */
    fun url(webAppBase: String, linkId: String, secret: ByteArray): String {
        val base = if (webAppBase.endsWith("/")) webAppBase else "$webAppBase/"
        return "$base?$PARAM=${URLEncoder.encode(linkId, "UTF-8")}#${encodeSecret(secret)}"
    }

    fun encodeSecret(secret: ByteArray): String = Base64.getUrlEncoder().withoutPadding().encodeToString(secret)

    /** The 32-byte secret in a link's fragment (with or without its "#"), or null. Like libsodium, only the one canonical spelling counts. */
    fun secretFromFragment(fragment: String): ByteArray? {
        val text = fragment.removePrefix("#")
        if (!BASE64URL.matches(text)) return null
        val bytes = runCatching { Base64.getUrlDecoder().decode(text) }.getOrNull() ?: return null
        return bytes.takeIf { it.size == SECRET_BYTES && encodeSecret(it) == text }
    }

    /** A pasted invite link, or the whole message it came in, checked for shape only. Null when it isn't one. */
    fun parse(text: String): Parsed? {
        val candidate = ADDRESS.find(text)?.value ?: text.trim()
        val linkId = queryParam(candidate, PARAM) ?: return null
        val secret = candidate.substringAfter('#', "")
        if (!LINK_ID.matches(linkId) || !SECRET.matches(secret)) return null
        return Parsed(linkId, secret)
    }

    /** The invite in [url] when it points at this app's own address ([webAppBase]), for a tap in a chat bubble. */
    fun parseOwn(url: String, webAppBase: String): Parsed? {
        val parsed = parse(url) ?: return null
        val own = webAppBase.trimEnd('/').substringBefore('?').lowercase()
        val mine = url.trim().substringBefore('?').substringBefore('#').trimEnd('/').lowercase()
        return parsed.takeIf { mine == own }
    }

    private fun queryParam(address: String, name: String): String? {
        val query = address.substringBefore('#').substringAfter('?', "").takeIf { it.isNotEmpty() } ?: return null
        for (pair in query.split('&')) {
            if (pair.substringBefore('=') == name) return runCatching { URLDecoder.decode(pair.substringAfter('=', ""), "UTF-8") }.getOrNull()
        }
        return null
    }
}

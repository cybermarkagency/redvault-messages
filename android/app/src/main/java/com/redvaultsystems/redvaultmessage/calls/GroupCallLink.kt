// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.calls

import java.net.URLDecoder
import java.net.URLEncoder

/**
 * Group call links: the address of a call that's going on in a group chat. A
 * port of the web's lib/groupCallLink.ts. The member who starts the call posts
 * it in the chat, and the others paste it into "Join with a link" (or tap it)
 * to join.
 *
 * There's no secret in it. The server only lets members of that group in
 * (anyone else is told the call has ended), and the call's media and keys are
 * end-to-end encrypted on their own (crypto/GroupCallCrypto.kt).
 */
object GroupCallLink {
    private const val PARAM = "group-call"
    private val CALL_ID = Regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", RegexOption.IGNORE_CASE)
    private val ADDRESS = Regex("https?://\\S+")

    /** The shareable address: the web app's, with the call's id. [webAppBase] is the site's address (…/redvaultmessages/). */
    fun url(webAppBase: String, callId: String): String {
        val base = if (webAppBase.endsWith("/")) webAppBase else "$webAppBase/"
        return "$base?$PARAM=${URLEncoder.encode(callId, "UTF-8")}"
    }

    /** What the starter's app posts in the chat, word for word the same as the web's. */
    fun inviteText(kind: String, url: String): String =
        "📞 Group $kind call started. To join, open Calls, tap \"Join with a link\" and paste this link:\n$url"

    /** The call id in a pasted group call link (or in the chat message that carries it), or null. */
    fun parse(text: String): String? {
        val candidate = ADDRESS.find(text)?.value ?: text.trim()
        val callId = queryParam(candidate, PARAM) ?: return null
        return callId.takeIf { CALL_ID.matches(it) }?.lowercase()
    }

    /**
     * The call id in [url] when it's a link to this app's own address ([webAppBase]), for a tap in a
     * chat bubble: only those join in the app, anything else is a link like any other.
     */
    fun parseOwn(url: String, webAppBase: String): String? {
        val callId = parse(url) ?: return null
        val own = webAppBase.trimEnd('/').substringBefore('?').lowercase()
        val mine = url.trim().substringBefore('?').substringBefore('#').trimEnd('/').lowercase()
        return callId.takeIf { mine == own }
    }

    private fun queryParam(address: String, name: String): String? {
        val withoutFragment = address.substringBefore('#')
        val query = withoutFragment.substringAfter('?', "").takeIf { it.isNotEmpty() } ?: return null
        for (pair in query.split('&')) {
            val key = pair.substringBefore('=')
            if (key == name) return runCatching { URLDecoder.decode(pair.substringAfter('=', ""), "UTF-8") }.getOrNull()
        }
        return null
    }
}

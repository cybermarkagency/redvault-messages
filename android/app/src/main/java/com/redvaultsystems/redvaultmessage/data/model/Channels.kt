// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

import com.redvaultsystems.redvaultmessage.crypto.ChannelCrypto
import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import kotlinx.serialization.Serializable
import java.net.URLEncoder
import java.time.Instant

/**
 * A channel followed on this phone. Kept only here, never on the server, so
 * nobody can list a channel's followers. [seenAt] is the newest post already
 * seen (a post's createdAt), for the unread dot. [secret] is a private
 * channel's link secret (base64url), which is what lets this phone read it;
 * [muted] turns off its new-post notifications (it still counts on the badge).
 * The web's shape: {"id","name","seenAt"[,"secret"][,"muted"]}.
 */
@Serializable
data class FollowedChannel(
    val id: String,
    val name: String,
    val seenAt: String? = null,
    val secret: String? = null,
    val muted: Boolean = false,
)

/** A channel link: the channel's id (lowercase) and, for a private channel, the secret from its fragment. */
data class ChannelLink(val id: String, val secret: String?)

/**
 * Port of the web's lib/channels.ts: following on this device, the unread
 * dot, and channel links (…/?channel=<id>, plus #<secret> for a private
 * channel, where the secret never reaches any server).
 */
object Channels {
    private const val PARAM = "channel="
    private val CHANNEL_ID = Regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", RegexOption.IGNORE_CASE)

    /** A channel's shareable link: the web app's address ([webAppBase], …/redvaultmessages/) with ?channel=<id>, and #<secret> when private. */
    fun url(webAppBase: String, id: String, secret: String? = null): String {
        val base = if (webAppBase.endsWith("/")) webAppBase else "$webAppBase/"
        val link = "$base?channel=${URLEncoder.encode(id, "UTF-8")}"
        return if (secret.isNullOrEmpty()) link else "$link#$secret"
    }

    /**
     * The channel a link points to (…?channel=<id>, with #<secret> for a
     * private one), found anywhere in [text] (a pasted message, say); null
     * for anything else. A fragment that isn't a secret is ignored, as the
     * web ignores it. The same rules as the desktop app's parseChannelLink.
     */
    fun parse(text: String): ChannelLink? {
        var at = text.indexOf(PARAM)
        while (at >= 0) {
            val start = at + PARAM.length
            val boundary = at > 0 && (text[at - 1] == '?' || text[at - 1] == '&')
            val id = if (start + 36 <= text.length) text.substring(start, start + 36) else ""
            val next = text.getOrNull(start + 36)
            val ends = next == null || !(next.isLetterOrDigit() && next.code < 128 || next == '-' || next == '_')
            if (boundary && CHANNEL_ID.matches(id) && ends) {
                // The rest of this link, up to whitespace: its fragment, like location.hash, is the secret if it's one.
                var end = start + 36
                while (end < text.length && !text[end].isWhitespace()) end++
                val rest = text.substring(start + 36, end)
                val secret = rest.indexOf('#').takeIf { it >= 0 }?.let { ChannelCrypto.secretFromFragment(rest.substring(it + 1)) }
                return ChannelLink(id.lowercase(), secret)
            }
            at = text.indexOf(PARAM, at + 1)
        }
        return null
    }

    /** The channel in [url] when it's a channel link to this app's own address, for a tap in a chat bubble. */
    fun parseOwn(url: String, webAppBase: String): ChannelLink? {
        val address = url.trim()
        val own = webAppBase.trimEnd('/').substringBefore('?').lowercase()
        if (address.substringBefore('?').substringBefore('#').trimEnd('/').lowercase() != own) return null
        return parse(address)
    }

    fun isFollowing(list: List<FollowedChannel>, id: String): Boolean = list.any { it.id == id }

    /** Following starts with everything already posted counted as seen. A private channel's secret comes along. */
    fun follow(list: List<FollowedChannel>, channel: ChannelDto, secret: String? = null): List<FollowedChannel> =
        if (isFollowing(list, channel.id)) list
        else list + FollowedChannel(channel.id, channel.name, channel.lastPostAt, secret?.takeIf { it.isNotEmpty() })

    fun unfollow(list: List<FollowedChannel>, id: String): List<FollowedChannel> = list.filter { it.id != id }

    /** Everything in the channel up to its newest post has been seen; also keeps its name (and a newer secret) current. */
    fun markSeen(list: List<FollowedChannel>, channel: ChannelDto, secret: String? = null): List<FollowedChannel> =
        list.map {
            if (it.id == channel.id) it.copy(name = channel.name, seenAt = channel.lastPostAt, secret = secret?.takeIf { s -> s.isNotEmpty() } ?: it.secret)
            else it
        }

    fun setMuted(list: List<FollowedChannel>, id: String, muted: Boolean): List<FollowedChannel> =
        list.map { if (it.id == id) it.copy(muted = muted) else it }

    fun hasUnread(followed: FollowedChannel, channel: ChannelDto): Boolean {
        val last = channel.lastPostAt ?: return false
        val seen = followed.seenAt ?: return true
        // Like the web's Date.parse: something that doesn't parse never counts as newer.
        val lastMs = epochMs(last) ?: return false
        val seenMs = epochMs(seen) ?: return false
        return lastMs > seenMs
    }

    fun epochMs(iso: String): Long? = runCatching { Instant.parse(iso).toEpochMilli() }.getOrNull()
}

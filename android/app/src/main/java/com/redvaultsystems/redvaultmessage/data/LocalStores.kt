package com.redvaultsystems.redvaultmessage.data

import android.content.Context
import com.redvaultsystems.redvaultmessage.data.model.FollowedChannel
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json

/** Local-only preferences. Read receipts default off, like the web client. */
class ChatSettingsStore(context: Context) {
    private val prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)

    var readReceipts: Boolean
        get() = prefs.getBoolean("readReceipts", false)
        set(value) = prefs.edit().putBoolean("readReceipts", value).apply()

    /** Blocks screenshots and blanks the app in the Recents view (FLAG_SECURE). */
    var hideContent: Boolean
        get() = prefs.getBoolean("hideContent", false)
        set(value) = prefs.edit().putBoolean("hideContent", value).apply()

    /** Ring for incoming calls (muted contacts never ring either way). On by default, like the desktop. */
    var ringtone: Boolean
        get() = prefs.getBoolean("ringtone", true)
        set(value) = prefs.edit().putBoolean("ringtone", value).apply()

    /**
     * Echo cancellation on calls (the phone's own canceller plus WebRTC's).
     * On by default; off only helps with a headset and a noisy line.
     */
    var echoCancellation: Boolean
        get() = prefs.getBoolean("echoCancellation", true)
        set(value) = prefs.edit().putBoolean("echoCancellation", value).apply()

    /** Whether the one-time notification permission prompt has been shown. */
    var askedForNotifications: Boolean
        get() = prefs.getBoolean("askedForNotifications", false)
        set(value) = prefs.edit().putBoolean("askedForNotifications", value).apply()
}

/**
 * Per-account choices that stay on this phone, like the desktop's prefs:
 * favorite and muted contacts, and whether typing indicators are on (off
 * by default; they're reciprocal: off means you neither send nor see them).
 */
class ContactPrefsStore(context: Context) {
    private val prefs = context.getSharedPreferences("contactPrefs", Context.MODE_PRIVATE)

    fun favorites(selfUserId: String): Set<String> = prefs.getStringSet("$selfUserId:favorites", emptySet()).orEmpty()

    fun muted(selfUserId: String): Set<String> = prefs.getStringSet("$selfUserId:muted", emptySet()).orEmpty()

    fun setFavorites(selfUserId: String, ids: Set<String>) = prefs.edit().putStringSet("$selfUserId:favorites", ids).apply()

    fun setMuted(selfUserId: String, ids: Set<String>) = prefs.edit().putStringSet("$selfUserId:muted", ids).apply()

    fun typingIndicators(selfUserId: String): Boolean = prefs.getBoolean("$selfUserId:typingIndicators", false)

    fun setTypingIndicators(selfUserId: String, on: Boolean) = prefs.edit().putBoolean("$selfUserId:typingIndicators", on).apply()
}

/**
 * "Delete for me": message ids hidden on this phone for this account. The
 * other person keeps the message and the server is never told.
 */
class HiddenMessagesStore(context: Context) {
    private val prefs = context.getSharedPreferences("hiddenMessages", Context.MODE_PRIVATE)

    fun load(selfUserId: String): Set<String> = prefs.getStringSet(selfUserId, emptySet()).orEmpty()

    fun hide(selfUserId: String, messageId: String) {
        val ids = (load(selfUserId) + messageId).toList().takeLast(MAX_IDS).toSet()
        prefs.edit().putStringSet(selfUserId, ids).apply()
    }

    private companion object {
        const val MAX_IDS = 5000
    }
}

/**
 * Port of ui/src/lib/verifiedContacts.ts: "have I confirmed this contact's
 * safety number?" It's a judgment about a public key, so it's never sent to
 * the server, and it's keyed by the key itself: if a contact's key changes,
 * old trust isn't silently inherited.
 */
class VerifiedContactsStore(context: Context) {
    private val prefs = context.getSharedPreferences("verifiedContacts", Context.MODE_PRIVATE)

    private fun key(selfUserId: String, contactPublicKeyB64: String) = "$selfUserId:$contactPublicKeyB64"

    fun isVerified(selfUserId: String, contactPublicKeyB64: String): Boolean =
        prefs.getBoolean(key(selfUserId, contactPublicKeyB64), false)

    fun setVerified(selfUserId: String, contactPublicKeyB64: String, verified: Boolean) {
        val editor = prefs.edit()
        if (verified) editor.putBoolean(key(selfUserId, contactPublicKeyB64), true) else editor.remove(key(selfUserId, contactPublicKeyB64))
        editor.apply()
    }
}

/**
 * Port of followedChannels in ui/src/lib/localPrefs.ts: the channels each
 * account follows on this phone, and which posts it has seen. Kept only
 * here, never sent to the server, so nobody can list a channel's followers.
 */
class FollowedChannelsStore(context: Context) {
    private val prefs = context.getSharedPreferences("followedChannels", Context.MODE_PRIVATE)
    private val serializer = ListSerializer(FollowedChannel.serializer())

    fun load(selfUserId: String): List<FollowedChannel> =
        prefs.getString(selfUserId, null)?.let { runCatching { json.decodeFromString(serializer, it) }.getOrNull() }.orEmpty()

    fun save(selfUserId: String, channels: List<FollowedChannel>) =
        prefs.edit().putString(selfUserId, json.encodeToString(serializer, channels)).apply()

    private companion object {
        val json = Json { ignoreUnknownKeys = true }
    }
}

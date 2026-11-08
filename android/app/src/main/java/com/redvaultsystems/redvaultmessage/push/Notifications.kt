// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.push

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.ProcessLifecycleOwner
import com.redvaultsystems.redvaultmessage.MainActivity
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.calls.CallNotifications
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive

/**
 * What a content-free push can say: only what kind of thing happened. There's no sender, chat or text in it
 * (and the private key isn't available to a push anyway), so the notification can't reveal anything a lock
 * screen shouldn't. The same kinds come through UnifiedPush ([UnifiedPushReceiver]) and the background
 * connection ([PushFallbackService]).
 */
object PushKinds {
    const val MESSAGE = "message"
    const val FRIEND_REQUEST = "friend_request"
    const val FRIEND_ACCEPTED = "friend_accepted"
    const val CALL = "call"
    const val SHARE_UPLOAD = "share_upload"

    private val json = Json { ignoreUnknownKeys = true }

    /** The kind in a decrypted Web Push payload (UTF-8 JSON `{"kind":"message"}`), or null if it isn't one we know. */
    fun parsePayload(content: ByteArray): String? {
        if (content.isEmpty() || content.size > 4096) return null
        val element = runCatching { json.parseToJsonElement(content.toString(Charsets.UTF_8)) }.getOrNull() as? JsonObject ?: return null
        val kind = (element["kind"] as? JsonPrimitive)?.takeIf { it.isString }?.content ?: return null
        return kind.takeIf { it == CALL || textFor(it) != null }
    }

    /** The notification text for [kind]; null for "call" (it rings instead) and for kinds this app doesn't know. */
    fun textFor(kind: String): String? = when (kind) {
        MESSAGE -> "You have a new message."
        FRIEND_REQUEST -> "You have a new friend request."
        FRIEND_ACCEPTED -> "Your friend request was accepted."
        // Files uploaded through a public share link (the push says no more: no link, name or count).
        SHARE_UPLOAD -> "New files arrived through one of your sharing links."
        else -> null
    }

    /**
     * Several kinds from one long poll come down to what to show: a call first (it rings), then the
     * newest other kind, since all of them update the one activity notification in place.
     */
    fun toShow(kinds: List<String>): List<String> {
        val known = kinds.filter { it == CALL || textFor(it) != null }
        val result = mutableListOf<String>()
        if (CALL in known) result += CALL
        known.lastOrNull { it != CALL }?.let { result += it }
        return result
    }

    /** Shows what [kind] calls for, unless the app is on screen (it's already refreshing; no need to buzz). */
    fun notify(context: Context, kind: String) {
        if (ProcessLifecycleOwner.get().lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) return
        if (kind == CALL) {
            // A call is ringing: opening the app picks it up from the live channel, which rings it.
            CallNotifications.notifyIncoming(context)
            return
        }
        val text = textFor(kind) ?: return
        Notifications.show(context, text, openChat = kind == MESSAGE)
    }
}

object Notifications {
    private const val CHANNEL_ID = "activity"
    private const val NOTIFICATION_ID = 1

    /**
     * On a "new message" notification's tap: open the chat. The push itself names no chat (a conversation
     * id would tell the push service which chat is busy, and, sent to every member's phone, who talks to
     * whom), so the app opens the one chat with unread messages, or the chat list when there are several.
     */
    const val EXTRA_OPEN_CHAT = "openChat"

    fun createChannel(context: Context) {
        if (Build.VERSION.SDK_INT < 26) return
        val channel = NotificationChannel(CHANNEL_ID, "Messages and friend requests", NotificationManager.IMPORTANCE_HIGH).apply {
            description = "Tells you something new arrived. Never shows who from or what it says."
            lockscreenVisibility = android.app.Notification.VISIBILITY_PRIVATE
        }
        context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    /** Android 13+ asks before an app may post notifications; earlier versions always allow it. */
    fun allowed(context: Context): Boolean =
        Build.VERSION.SDK_INT < 33 ||
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED

    fun show(context: Context, text: String, openChat: Boolean = false) {
        if (!allowed(context)) return
        val open = PendingIntent.getActivity(
            context,
            0,
            Intent(context, MainActivity::class.java)
                .putExtra(EXTRA_OPEN_CHAT, openChat)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val notification = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(context.getString(R.string.app_name))
            .setContentText(text)
            .setContentIntent(open)
            .setAutoCancel(true)
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setCategory(NotificationCompat.CATEGORY_MESSAGE)
            .build()
        // One notification that updates in place, however many pushes arrive.
        NotificationManagerCompat.from(context).notify(NOTIFICATION_ID, notification)
    }
}

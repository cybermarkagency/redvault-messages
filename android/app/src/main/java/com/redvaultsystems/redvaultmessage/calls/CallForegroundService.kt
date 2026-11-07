package com.redvaultsystems.redvaultmessage.calls

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.ProcessLifecycleOwner
import com.redvaultsystems.redvaultmessage.MainActivity
import com.redvaultsystems.redvaultmessage.R

/**
 * Keeps a call going while the app is in the background (Android stops
 * the microphone, camera and network for apps that aren't visible
 * otherwise), with the ongoing "Encrypted call" notification. It holds
 * only the types the call is using right now: microphone, plus camera
 * while it's on, plus media projection while the screen is shared.
 */
class CallForegroundService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val video = intent?.getBooleanExtra(EXTRA_VIDEO, false) == true
        val screen = intent?.getBooleanExtra(EXTRA_SCREEN, false) == true
        val notification = CallNotifications.ongoing(this)
        if (Build.VERSION.SDK_INT >= 30) {
            var types = ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            if (video && granted(Manifest.permission.CAMERA)) types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
            if (screen) types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            startForeground(CallNotifications.ONGOING_ID, notification, types)
        } else {
            startForeground(CallNotifications.ONGOING_ID, notification)
        }
        return START_NOT_STICKY
    }

    private fun granted(permission: String) = ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED

    companion object {
        private const val EXTRA_VIDEO = "video"
        private const val EXTRA_SCREEN = "screen"

        /** Starts the service, or updates which kinds of media it holds. */
        fun update(context: Context, video: Boolean, screen: Boolean) {
            val intent = Intent(context, CallForegroundService::class.java).putExtra(EXTRA_VIDEO, video).putExtra(EXTRA_SCREEN, screen)
            runCatching { ContextCompat.startForegroundService(context, intent) }
        }

        fun stop(context: Context) {
            runCatching { context.stopService(Intent(context, CallForegroundService::class.java)) }
            CallNotifications.cancelIncoming(context)
        }
    }
}

/** The call notifications: the ongoing call, and "Incoming call" when the app isn't on screen. Never who's calling. */
object CallNotifications {
    const val ONGOING_ID = 20
    private const val INCOMING_ID = 21
    private const val ONGOING_CHANNEL = "call-ongoing"
    const val INCOMING_CHANNEL = "calls"

    fun createChannels(context: Context) {
        if (Build.VERSION.SDK_INT < 26) return
        val manager = context.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(ONGOING_CHANNEL, "Ongoing call", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Shown while you're on a call, so it keeps going in the background."
                setShowBadge(false)
            },
        )
        manager.createNotificationChannel(
            NotificationChannel(INCOMING_CHANNEL, "Incoming calls", NotificationManager.IMPORTANCE_HIGH).apply {
                description = "Rings when someone calls you. Never shows who."
                lockscreenVisibility = Notification.VISIBILITY_PRIVATE
            },
        )
    }

    private fun openApp(context: Context): PendingIntent = PendingIntent.getActivity(
        context,
        0,
        Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
    )

    fun ongoing(context: Context): Notification {
        createChannels(context)
        return NotificationCompat.Builder(context, ONGOING_CHANNEL)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle("Encrypted call")
            .setContentText("Tap to return to the call")
            .setOngoing(true)
            .setCategory(NotificationCompat.CATEGORY_CALL)
            .setContentIntent(openApp(context))
            .setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
            .build()
    }

    /** "Incoming call", only when the app isn't on screen (on screen, the call screen rings). */
    fun showIncoming(context: Context, @Suppress("UNUSED_PARAMETER") handle: String, video: Boolean) {
        if (ProcessLifecycleOwner.get().lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) return
        notifyIncoming(context, if (video) "Incoming video call" else "Incoming call")
    }

    /** Also used by a content-free "call" push, which can't say what kind. */
    fun notifyIncoming(context: Context, text: String = "Incoming call") {
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        createChannels(context)
        val notification = NotificationCompat.Builder(context, INCOMING_CHANNEL)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(context.getString(R.string.app_name))
            .setContentText(text)
            .setCategory(NotificationCompat.CATEGORY_CALL)
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setAutoCancel(true)
            .setTimeoutAfter(45_000)
            .setContentIntent(openApp(context))
            .setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
            .build()
        context.getSystemService(NotificationManager::class.java).notify(INCOMING_ID, notification)
    }

    fun cancelIncoming(context: Context) {
        context.getSystemService(NotificationManager::class.java).cancel(INCOMING_ID)
    }
}

package com.redvaultsystems.redvaultmessage.ui.channels

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
import com.redvaultsystems.redvaultmessage.MainActivity
import com.redvaultsystems.redvaultmessage.R

/**
 * New-post notifications for followed channels. The app works these out
 * itself (ChannelSession checks each followed channel with public reads), so
 * the server still has no follower list and sends no channel pushes; they
 * show while the app is running. Channels are public, so a notification may
 * show the post; on the lock screen only "New post in a channel you follow"
 * shows, and with app lock on, that's all it ever shows.
 */
object ChannelNotifications {
    private const val CHANNEL_ID = "channels"
    /** Set on the tap's intent: the channel to open. */
    const val EXTRA_CHANNEL_ID = "openChannel"

    fun createChannel(context: Context) {
        if (Build.VERSION.SDK_INT < 26) return
        val channel = NotificationChannel(CHANNEL_ID, "New posts in channels", NotificationManager.IMPORTANCE_DEFAULT).apply {
            description = "A channel you follow posted something. Worked out on this phone while the app is running."
            lockscreenVisibility = android.app.Notification.VISIBILITY_PRIVATE
        }
        context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    fun allowed(context: Context): Boolean =
        Build.VERSION.SDK_INT < 33 || ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED

    fun show(context: Context, notice: ChannelNotice, hideContent: Boolean) {
        if (!allowed(context)) return
        val open = PendingIntent.getActivity(
            context,
            notice.channelId.hashCode(),
            Intent(context, MainActivity::class.java)
                .putExtra(EXTRA_CHANNEL_ID, notice.channelId)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val generic = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(context.getString(R.string.app_name))
            .setContentText("New post in a channel you follow")
            .build()
        val notification = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(if (hideContent) context.getString(R.string.app_name) else notice.channelName)
            .setContentText(if (hideContent) "New post in a channel you follow" else notice.preview)
            .setPublicVersion(generic)
            .setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
            .setContentIntent(open)
            .setAutoCancel(true)
            .setCategory(NotificationCompat.CATEGORY_SOCIAL)
            .build()
        // One per channel, replaced by its next post.
        runCatching { NotificationManagerCompat.from(context).notify("channel-${notice.channelId}", 0, notification) }
    }
}

package com.redvaultsystems.redvaultmessage.push

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import com.redvaultsystems.redvaultmessage.MainActivity
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.RedvaultApp
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * The background connection, for phones without a UnifiedPush distributor (or when the server can't send
 * Web Push): a foreground service that long-polls `GET /api/push/wait` through the app's own API client,
 * so Tor routing, the session cookie and the own-server-only rule all apply as for every other request.
 * It shows the same content-free notifications a push would, and backs off on errors (doubling, at most
 * about 5 minutes).
 *
 * It runs only while someone is signed in in this process: the session lives in memory (nothing is
 * persisted across process death), so it's START_NOT_STICKY and there's no boot receiver. After a reboot
 * or a killed process the next sign-in starts it again.
 */
class PushFallbackService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var loop: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = ongoing(this)
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(ONGOING_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
            } else {
                startForeground(ONGOING_ID, notification)
            }
        } catch (_: IllegalStateException) {
            // ForegroundServiceStartNotAllowedException (Android 12+): not allowed from the background right
            // now. The registrar tries again on the next foreground.
            running = false
            stopSelf()
            return START_NOT_STICKY
        }
        running = true
        val container = (application as RedvaultApp).container
        if (!container.hasActiveSession) {
            stopEverything()
            return START_NOT_STICKY
        }
        if (loop?.isActive != true) {
            val registrar = container.pushRegistrar
            loop = scope.launch {
                val end = runWaitLoop(
                    fetch = { after -> apiCall(ApiClient.json) { registrar.api.wait(after) } },
                    show = { kinds -> kinds.forEach { PushKinds.notify(this@PushFallbackService, it) } },
                    sleep = { delay(it) },
                    keepGoing = { isActive && container.hasActiveSession },
                )
                if (end == WaitEnd.SignedOut) registrar.fallbackSignedOut()
                stopEverything()
            }
        }
        return START_NOT_STICKY
    }

    private fun stopEverything() {
        running = false
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        running = false
        scope.cancel()
        super.onDestroy()
    }

    companion object {
        private const val ONGOING_ID = 30
        private const val CHANNEL_ID = "background-connection"

        /** True while the service holds the connection. */
        @Volatile
        var running = false
            private set

        fun createChannel(context: Context) {
            if (Build.VERSION.SDK_INT < 26) return
            val channel = NotificationChannel(CHANNEL_ID, "Background connection", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Keeps RedVault connected so you hear about new messages."
                setShowBadge(false)
            }
            context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }

        private fun ongoing(context: Context): Notification {
            createChannel(context)
            val open = PendingIntent.getActivity(
                context,
                0,
                Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
            return NotificationCompat.Builder(context, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_notification)
                .setContentTitle("Background connection")
                .setContentText("Keeps RedVault connected so you hear about new messages.")
                .setOngoing(true)
                .setSilent(true)
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .setCategory(NotificationCompat.CATEGORY_SERVICE)
                .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
                .setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
                .setContentIntent(open)
                .build()
        }

        /**
         * Starts the connection. False when Android doesn't allow it right now (Android 12+ won't start a
         * foreground service from the background: then the registrar tries again on the next foreground).
         */
        fun start(context: Context): Boolean {
            if (running) return true
            return try {
                ContextCompat.startForegroundService(context, Intent(context, PushFallbackService::class.java))
                true
            } catch (_: IllegalStateException) {
                // ForegroundServiceStartNotAllowedException (API 31+) is one of these, as is the older
                // "not allowed to start service: app is in background".
                false
            } catch (_: SecurityException) {
                false
            }
        }

        fun stop(context: Context) {
            runCatching { context.stopService(Intent(context, PushFallbackService::class.java)) }
            running = false
        }
    }
}

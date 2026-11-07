package com.redvaultsystems.redvaultmessage

import android.content.Intent
import android.os.Bundle
import android.view.MotionEvent
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.SystemBarStyle
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.redvaultsystems.redvaultmessage.push.Notifications
import com.redvaultsystems.redvaultmessage.ui.AuthStatus
import com.redvaultsystems.redvaultmessage.ui.AuthViewModel
import com.redvaultsystems.redvaultmessage.ui.SignedInApp
import com.redvaultsystems.redvaultmessage.ui.channels.ChannelNotifications
import com.redvaultsystems.redvaultmessage.ui.components.LocalAvatarLoader
import com.redvaultsystems.redvaultmessage.ui.onboarding.OnboardingScreen
import com.redvaultsystems.redvaultmessage.ui.recovery.RecoveryKeyIssuedScreen
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import com.redvaultsystems.redvaultmessage.ui.verify.TransparencyFeedScreen

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The design is dark only: light status and navigation bar icons.
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
        )
        val container = (application as RedvaultApp).container
        takeChannelToOpen(intent)
        // Not again after a rotation or a restore: the same intent comes back with the saved state.
        if (savedInstanceState == null) takeChatOrShare(intent)

        setContent {
            RedvaultTheme {
                CompositionLocalProvider(LocalAvatarLoader provides container.avatarLoader) {
                    val authViewModel: AuthViewModel = viewModel(
                        factory = viewModelFactory {
                            initializer { AuthViewModel(container.authRepository, container::createSession) }
                        },
                    )
                    val state by authViewModel.state.collectAsStateWithLifecycle()
                    val session = state.session

                    var showFeed by rememberSaveable { mutableStateOf(false) }

                    val issuedKey = state.issuedRecoveryKey
                    if (state.status == AuthStatus.SignedIn && session != null && issuedKey != null) {
                        RecoveryKeyIssuedScreen(issuedKey, onDone = authViewModel::recoveryKeyAcknowledged)
                    } else if (state.status == AuthStatus.SignedIn && session != null) {
                        SignedInApp(
                            session,
                            loadTransparencyFeed = container::transparencyFeed,
                            onLogout = { authViewModel.logout() },
                            openChannelRequests = container.openChannelRequests,
                            openChatRequests = container.openChatRequests,
                            shareRequests = container.shareRequests,
                        )
                    } else if (showFeed) {
                        BackHandler { showFeed = false }
                        TransparencyFeedScreen(loadFeed = container::transparencyFeed, onBack = { showFeed = false })
                    } else {
                        OnboardingScreen(
                            state = state,
                            onRegister = authViewModel::register,
                            onLogin = authViewModel::login,
                            onSubmitMfa = authViewModel::submitMfaCode,
                            onCancelMfa = authViewModel::cancelMfa,
                            onModeChange = authViewModel::setMode,
                            onRegenerateHandle = authViewModel::regenerateCandidate,
                            onOpenTransparency = { showFeed = true },
                            onRecover = authViewModel::recover,
                        )
                    }
                }
            }
        }
    }

    /** A sensitive screen drops touches that come through another app's window (TouchGuard): say why nothing happened. */
    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        val obscured = event.flags and MotionEvent.FLAG_WINDOW_IS_OBSCURED != 0
        if (obscured && event.actionMasked == MotionEvent.ACTION_DOWN && window.decorView.filterTouchesWhenObscured) {
            android.widget.Toast.makeText(this, "Another app is drawing over RedVault. Close it, then try again.", android.widget.Toast.LENGTH_LONG).show()
        }
        return super.dispatchTouchEvent(event)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        takeChannelToOpen(intent)
        takeChatOrShare(intent)
    }

    /**
     * A tapped "new message" notification (the signed-in app picks the chat), or "Share to RedVault" from
     * another app (the signed-in app asks which chat). Either waits behind sign-in and the lock screen.
     * This activity is singleTask, so a share arrives here, in the app's own task, and the one-time read
     * grant for the shared files belongs to it for as long as it lives.
     */
    private fun takeChatOrShare(intent: Intent?) {
        intent ?: return
        val container = (application as RedvaultApp).container
        if (intent.getBooleanExtra(Notifications.EXTRA_OPEN_CHAT, false)) {
            intent.removeExtra(Notifications.EXTRA_OPEN_CHAT)
            container.openChatRequests.value = true
        }
        if (intent.action == Intent.ACTION_SEND || intent.action == Intent.ACTION_SEND_MULTIPLE) {
            val items = ShareIntents.itemsFrom(this, intent)
            if (items != null) container.shareRequests.value = items
            else android.widget.Toast.makeText(this, "Nothing in that can be sent with RedVault.", android.widget.Toast.LENGTH_LONG).show()
            intent.action = Intent.ACTION_MAIN
        }
    }

    /** A tapped new-post notification: the signed-in app opens that channel (after unlocking, if app lock is on). */
    private fun takeChannelToOpen(intent: Intent?) {
        val id = intent?.getStringExtra(ChannelNotifications.EXTRA_CHANNEL_ID) ?: return
        intent.removeExtra(ChannelNotifications.EXTRA_CHANNEL_ID)
        if (Regex("^[0-9a-f-]{36}$", RegexOption.IGNORE_CASE).matches(id)) (application as RedvaultApp).container.openChannelRequests.value = id.lowercase()
    }
}

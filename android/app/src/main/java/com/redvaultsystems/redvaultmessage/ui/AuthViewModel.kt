// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui

import com.redvaultsystems.redvaultmessage.ui.passwords.Biometrics
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.UnsafeKdfException
import com.redvaultsystems.redvaultmessage.data.AuthRepository
import com.redvaultsystems.redvaultmessage.data.LoginOutcome
import com.redvaultsystems.redvaultmessage.data.RecoverOutcome
import com.redvaultsystems.redvaultmessage.data.SignupConsent
import com.redvaultsystems.redvaultmessage.data.UserFacingException
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.TOR_UNAVAILABLE_MESSAGE
import com.redvaultsystems.redvaultmessage.identity.HandleGenerator
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import com.redvaultsystems.redvaultmessage.tor.TorUnavailableException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

enum class AuthStatus { SignedOut, Authenticating, MfaPending, SignedIn }

enum class OnboardingMode { Create, Login, Recover }

data class AuthUiState(
    val status: AuthStatus = AuthStatus.SignedOut,
    val mode: OnboardingMode = OnboardingMode.Create,
    val lastHandle: String? = null,
    val candidateHandle: String = "",
    val error: String? = null,
    val identity: SelfIdentity? = null,
    /** Present exactly while signed in. */
    val session: SignedInSession? = null,
    /** A recovery key just created at sign-up: shown once, then cleared. */
    val issuedRecoveryKey: String? = null,
    /** Recovery needs the two-factor code for this account. */
    val recoverNeedsCode: Boolean = false,
    /**
     * Sign-in (or its two-factor code) couldn't go out because Tor isn't connected yet: it goes as soon as Tor is
     * (no error shown meanwhile). Cancel ([AuthViewModel.cancelWaitForTor]) goes back to the form.
     */
    val waitingForTor: Boolean = false,
)

/** Whether [e] means Tor itself isn't connected, so nothing was sent (data/api/ApiException.kt, tor/TorRouting.kt). */
internal fun isTorUnavailable(e: Throwable): Boolean =
    e is TorUnavailableException || (e is ApiException && e.code == "tor_unavailable")

/**
 * Runs [attempt]; while it fails only because Tor isn't connected, says so through [waiting] (true), waits for Tor
 * ([waitForTor]) and tries again, at most once per [pauseMillis]. Any other outcome ends it ([waiting] false first).
 * Cancelling the coroutine stops the wait.
 */
internal suspend fun <T> retryWhenTorConnects(
    waitForTor: suspend () -> Unit,
    waiting: (Boolean) -> Unit,
    pauseMillis: Long = TOR_RETRY_PAUSE_MILLIS,
    attempt: suspend () -> T,
): T {
    while (true) {
        try {
            return attempt().also { waiting(false) }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (!isTorUnavailable(e)) {
                waiting(false)
                throw e
            }
        }
        waiting(true)
        waitForTor()
        // Tor may say "connected" a moment before its gate takes requests: never a tight loop.
        delay(pauseMillis)
    }
}

internal const val TOR_RETRY_PAUSE_MILLIS = 1_000L

/**
 * Port of the auth flow in ui/src/state (reducer + actions). Owns the
 * [SignedInSession], which is created on sign-in and closed (keys wiped) on
 * logout or when the server says the session has expired.
 */
class AuthViewModel(
    private val repository: AuthRepository,
    private val createSession: (identity: SelfIdentity, onSessionExpired: () -> Unit) -> SignedInSession,
    /** Returns once Tor is connected (AppContainer.waitForTor); null without Tor routing: nothing waits. */
    private val waitForTor: (suspend () -> Unit)? = null,
) : ViewModel() {

    private val _state = MutableStateFlow(
        repository.lastHandle.let { last ->
            // Someone has signed in on this device before: land on Log in
            // with their handle ready, not on Create (see LastHandleStore).
            AuthUiState(
                mode = if (last != null) OnboardingMode.Login else OnboardingMode.Create,
                lastHandle = last,
                candidateHandle = HandleGenerator.generate(),
            )
        },
    )
    val state: StateFlow<AuthUiState> = _state.asStateFlow()

    // Kept only between a login that answered "MFA required" and the code
    // being verified; the key can't be unwrapped without the password.
    private var pendingLogin: Pair<String, String>? = null

    /** The sign-in or code check that may wait for Tor, so Cancel can stop it. */
    private var waiting: Job? = null

    /** Runs [attempt], waiting for Tor and trying again while Tor isn't connected (when this app routes through Tor). */
    private suspend fun <T> whenTorConnects(attempt: suspend () -> T): T {
        val wait = waitForTor ?: return attempt()
        return retryWhenTorConnects(wait, { on -> _state.update { it.copy(waitingForTor = on) } }, attempt = attempt)
    }

    /** "Cancel" while waiting for Tor: stop, and go back to the form (or the code screen) as it was. */
    fun cancelWaitForTor() {
        val job = waiting ?: return
        waiting = null
        job.cancel()
        _state.update {
            it.copy(
                status = if (it.status == AuthStatus.MfaPending) AuthStatus.MfaPending else AuthStatus.SignedOut,
                waitingForTor = false,
                error = null,
            )
        }
    }

    fun setMode(mode: OnboardingMode) = _state.update { it.copy(mode = mode, error = null, recoverNeedsCode = false) }

    fun regenerateCandidate() = _state.update { it.copy(candidateHandle = HandleGenerator.generate()) }

    fun register(handle: String, password: String, consent: SignupConsent) = authenticate {
        val registered = repository.register(handle, password, consent)
        signedIn(registered.identity)
        _state.update { it.copy(issuedRecoveryKey = registered.recoveryKey) }
    }

    /** The user confirmed they saved the recovery key shown after sign-up. */
    fun recoveryKeyAcknowledged() = _state.update { it.copy(issuedRecoveryKey = null) }

    fun recover(handle: String, recoveryKey: String, newPassword: String, code: String?) = authenticate(recovering = true) {
        when (val outcome = repository.recover(handle, recoveryKey, newPassword, code)) {
            is RecoverOutcome.SignedIn -> signedIn(outcome.identity)
            RecoverOutcome.CodeRequired -> _state.update {
                it.copy(status = AuthStatus.SignedOut, recoverNeedsCode = true, error = "Enter the 6-digit code from your authenticator app.")
            }
        }
    }

    fun login(handle: String, password: String) = authenticate(genericError = WRONG_CREDENTIALS, waitsForTor = true) {
        // While it waits for Tor, the password stays only here, as it does for the whole sign-in; Cancel drops it.
        when (val outcome = whenTorConnects { repository.login(handle, password) }) {
            is LoginOutcome.SignedIn -> signedIn(outcome.identity)
            LoginOutcome.MfaRequired -> {
                pendingLogin = handle to password
                _state.update { it.copy(status = AuthStatus.MfaPending, error = null) }
            }
        }
    }

    fun submitMfaCode(code: String) {
        val (handle, password) = pendingLogin ?: return
        // Cleared first so a repeat of the same error still registers as a
        // change (the code screen re-enables Verify when an error arrives).
        _state.update { it.copy(error = null) }
        if (waiting?.isActive == true) return
        waiting = viewModelScope.launch {
            try {
                signedIn(whenTorConnects { repository.verifyMfa(handle, password, code) })
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                // Stays on the code screen rather than bouncing back to sign-in.
                _state.update { it.copy(status = AuthStatus.MfaPending, waitingForTor = false, error = describe(e, "Wrong code. Try again.")) }
            }
        }
    }

    fun cancelMfa() {
        waiting?.cancel()
        waiting = null
        pendingLogin = null
        _state.update { it.copy(status = AuthStatus.SignedOut, waitingForTor = false, error = null) }
    }

    fun logout(message: String? = null) {
        val current = _state.value
        if (current.status != AuthStatus.SignedIn) return
        current.session?.close()
        // The vault's biometric key belongs to this sign-in; the next one makes its own after its password (RV-1115-14).
        Biometrics.forget()
        // Leave the signed-in screens at once; the server call is best-effort.
        _state.update {
            AuthUiState(
                mode = OnboardingMode.Login,
                lastHandle = repository.lastHandle,
                candidateHandle = HandleGenerator.generate(),
                error = message,
            )
        }
        viewModelScope.launch { repository.logout(current.identity) }
    }

    override fun onCleared() {
        _state.value.session?.close()
        _state.value.identity?.wipe()
    }

    private fun signedIn(identity: SelfIdentity) {
        pendingLogin = null
        val session = createSession(identity) { logout(SESSION_EXPIRED) }
        _state.update {
            it.copy(
                status = AuthStatus.SignedIn,
                identity = identity,
                session = session,
                lastHandle = identity.handle,
                error = null,
                recoverNeedsCode = false,
            )
        }
    }

    private fun authenticate(genericError: String? = null, recovering: Boolean = false, waitsForTor: Boolean = false, block: suspend () -> Unit) {
        if (_state.value.status == AuthStatus.Authenticating) return
        _state.update { it.copy(status = AuthStatus.Authenticating, error = null) }
        val job = viewModelScope.launch {
            try {
                block()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                val message = when {
                    // Tor isn't connected, so nothing was sent: never "wrong password".
                    isTorUnavailable(e) -> (e as? ApiException)?.message ?: TOR_UNAVAILABLE_MESSAGE
                    // Network trouble is worth saying plainly; anything else on
                    // login gets one message whether the handle doesn't exist,
                    // the password was rejected, or the key failed to unwrap,
                    // so it never leaks which one.
                    e is ApiException && e.code == "network_error" -> e.message
                    recovering && e is UserFacingException -> e.message
                    // The server's sign-in details are refused, so nothing was tried: not a wrong password.
                    e is UnsafeKdfException || e.message == E2eCrypto.SIGN_IN_DETAILS_WRONG -> e.message
                    genericError != null -> genericError
                    else -> describe(e, "Couldn't create that identity.")
                }
                _state.update { it.copy(status = AuthStatus.SignedOut, waitingForTor = false, error = message) }
            }
        }
        if (waitsForTor) waiting = job
    }

    private fun describe(e: Exception, fallback: String): String =
        (e as? UnsafeKdfException)?.message ?: (e as? ApiException)?.message?.takeIf { it.isNotBlank() } ?: fallback

    private companion object {
        const val WRONG_CREDENTIALS = "Wrong handle or password."
        const val SESSION_EXPIRED = "Your session expired. Log in again."
    }
}

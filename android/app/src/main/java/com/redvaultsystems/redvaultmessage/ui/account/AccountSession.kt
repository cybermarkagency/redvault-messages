// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.account

import android.net.Uri
import com.redvaultsystems.redvaultmessage.data.AccountRepository
import com.redvaultsystems.redvaultmessage.data.ContactPreview
import com.redvaultsystems.redvaultmessage.data.FriendRepository
import com.redvaultsystems.redvaultmessage.data.FriendRequestView
import com.redvaultsystems.redvaultmessage.data.FileRepository
import com.redvaultsystems.redvaultmessage.data.FriendRequests
import com.redvaultsystems.redvaultmessage.data.api.BlockedUserDto
import com.redvaultsystems.redvaultmessage.data.api.FileStorageUsageDto
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.MfaSetupResult
import com.redvaultsystems.redvaultmessage.data.api.PlanStatusDto
import com.redvaultsystems.redvaultmessage.identity.LastHandleStore
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

data class Profile(
    val handle: String,
    val avatarUrl: String?,
    val mfaEnabled: Boolean,
    val subscriptionTier: String,
    /** Server-side settings; null until /auth/me has answered. */
    val acceptMessageRequests: Boolean? = null,
    val transparencyMode: Boolean? = null,
    /** Whether contacts see when you're online (and so whether you see theirs). */
    val showOnlineStatus: Boolean? = null,
    /** Null until /auth/me has answered. */
    val hasRecoveryKey: Boolean? = null,
)

data class AccountState(
    val profile: Profile,
    val friendRequests: FriendRequests = FriendRequests(),
    /** People this user has blocked. */
    val blocked: List<BlockedUserDto> = emptyList(),
)

/**
 * The signed-in user's own account: profile, pending friend requests, and
 * self-service actions. The suspend actions throw on failure with a message
 * that's safe to show; a 401 also ends the session.
 */
class AccountSession(
    private val identity: SelfIdentity,
    private val friends: FriendRepository,
    private val account: AccountRepository,
    private val lastHandleStore: LastHandleStore,
    private val files: FileRepository,
    private val onSessionExpired: () -> Unit,
    private val onConversationsChanged: suspend () -> Unit,
) {
    private val _state = MutableStateFlow(
        AccountState(Profile(identity.handle, identity.avatarUrl, identity.mfaEnabled, identity.subscriptionTier)),
    )
    val state: StateFlow<AccountState> = _state.asStateFlow()

    /** Refreshes pending friend requests every 5 s and the profile (incl. plan) every minute, until cancelled. */
    suspend fun poll() {
        var tick = 0
        while (true) {
            if (tick % PROFILE_EVERY_TICKS == 0) {
                quietly { refreshProfile() }
                quietly { refreshBlocked() }
            }
            quietly { refreshRequests() }
            tick++
            delay(POLL_MS)
        }
    }

    private suspend fun refreshProfile() {
        val me = account.me()
        updateProfile {
            it.copy(
                handle = me.username,
                avatarUrl = me.avatarUrl,
                mfaEnabled = me.mfaEnabled,
                subscriptionTier = me.subscriptionTier,
                acceptMessageRequests = me.acceptMessageRequests,
                transparencyMode = me.transparencyMode,
                showOnlineStatus = me.showOnlineStatus,
                hasRecoveryKey = me.hasRecoveryKey,
            )
        }
    }

    suspend fun storageUsage(): FileStorageUsageDto = call { files.storageUsage() }

    suspend fun planStatus(): PlanStatusDto = call { account.planStatus() }

    /** Switch to Free at the end of the paid period. */
    suspend fun cancelPlan(): PlanStatusDto = call { account.cancelPlan() }

    /** Undoes cancelPlan() while the period lasts. */
    suspend fun resumePlan(): PlanStatusDto = call { account.resumePlan() }

    // --- Friends ---

    suspend fun lookup(handle: String): ContactPreview = call { friends.lookup(handle, identity.userId) }

    suspend fun sendFriendRequest(handle: String) = call {
        friends.sendRequest(handle)
        refreshRequests()
    }

    /** Returns the id of the chat the acceptance created. */
    suspend fun accept(request: FriendRequestView): String = call {
        val conversationId = friends.accept(identity, request)
        refreshRequests()
        onConversationsChanged()
        conversationId
    }

    suspend fun deny(request: FriendRequestView) = call {
        friends.deny(request.id)
        refreshRequests()
    }

    suspend fun cancel(request: FriendRequestView) = call {
        friends.cancel(request.id)
        refreshRequests()
    }

    // --- Settings ---

    suspend fun setAcceptMessageRequests(value: Boolean) = optimistic(
        apply = { it.copy(acceptMessageRequests = value) },
        revert = { it.copy(acceptMessageRequests = !value) },
    ) { account.setAcceptMessageRequests(value) }

    suspend fun setTransparencyMode(value: Boolean) = optimistic(
        apply = { it.copy(transparencyMode = value) },
        revert = { it.copy(transparencyMode = !value) },
    ) { account.setTransparencyMode(value) }

    suspend fun setShowOnlineStatus(value: Boolean) = optimistic(
        apply = { it.copy(showOnlineStatus = value) },
        revert = { it.copy(showOnlineStatus = !value) },
    ) { account.setShowOnlineStatus(value) }

    suspend fun changePassword(currentPassword: String, newPassword: String) = call {
        account.changePassword(identity, currentPassword, newPassword)
    }

    suspend fun changeUsername(username: String) = call {
        account.changeUsername(username, previous = identity.handle)
        lastHandleStore.save(username)
        updateProfile { it.copy(handle = username) }
    }

    suspend fun createRecoveryKey(currentPassword: String): String = call {
        account.createRecoveryKey(identity, currentPassword).also { updateProfile { p -> p.copy(hasRecoveryKey = true) } }
    }

    suspend fun startMfaSetup(): MfaSetupResult = call { account.startMfaSetup() }

    suspend fun confirmMfa(code: String) = call {
        account.confirmMfa(code)
        updateProfile { it.copy(mfaEnabled = true) }
    }

    suspend fun deleteAccount(password: String) = call { account.deleteAccount(identity, password) }

    suspend fun disableMfa(password: String) = call {
        account.disableMfa(identity, password)
        updateProfile { it.copy(mfaEnabled = false) }
    }

    suspend fun uploadAvatar(image: Uri) = call {
        val url = account.uploadAvatar(image)
        updateProfile { it.copy(avatarUrl = url) }
    }

    suspend fun removeAvatar() = call {
        account.removeAvatar()
        updateProfile { it.copy(avatarUrl = null) }
    }

    suspend fun report(userId: String, reason: String) = call { account.report(userId, reason) }

    /** Neither side can message the other or send friend requests until unblocked (enforced by the server). */
    suspend fun block(userId: String) = call {
        account.block(userId)
        refreshBlocked()
        refreshRequests()
    }

    suspend fun unblock(userId: String) = call {
        account.unblock(userId)
        refreshBlocked()
    }

    private suspend fun refreshBlocked() {
        val list = account.listBlocked()
        _state.update { it.copy(blocked = list) }
    }

    private suspend fun refreshRequests() {
        val requests = friends.pending()
        _state.update { it.copy(friendRequests = requests) }
    }

    private fun updateProfile(transform: (Profile) -> Profile) = _state.update { it.copy(profile = transform(it.profile)) }

    private suspend fun optimistic(apply: (Profile) -> Profile, revert: (Profile) -> Profile, block: suspend () -> Unit) {
        updateProfile(apply)
        try {
            call { block() }
        } catch (e: Exception) {
            updateProfile(revert)
            throw e
        }
    }

    private suspend fun <T> call(block: suspend () -> T): T {
        try {
            return block()
        } catch (e: ApiException) {
            // A wrong password or 2FA code is also a 401, but the session is fine.
            if (e.status == 401 && e.code != "invalid_credentials" && e.code != "invalid_code") onSessionExpired()
            throw e
        }
    }

    /** Background refreshes: failures are retried on the next tick, except an expired session. */
    private suspend fun quietly(block: suspend () -> Unit) {
        try {
            call(block)
        } catch (e: CancellationException) {
            throw e
        } catch (_: Exception) {
        }
    }

    private companion object {
        const val POLL_MS = 5_000L
        const val PROFILE_EVERY_TICKS = 12
    }
}

package com.redvaultsystems.redvaultmessage.data

import android.content.ContentResolver
import android.net.Uri
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.RecoveryKey
import com.redvaultsystems.redvaultmessage.data.api.SetRecoveryRequest
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.AvatarUploadRequest
import com.redvaultsystems.redvaultmessage.data.api.BlockRequest
import com.redvaultsystems.redvaultmessage.data.api.BlockedUserDto
import com.redvaultsystems.redvaultmessage.data.api.ChangePasswordRequest
import com.redvaultsystems.redvaultmessage.data.api.MeResult
import com.redvaultsystems.redvaultmessage.data.api.MfaCodeRequest
import com.redvaultsystems.redvaultmessage.data.api.MfaSetupResult
import com.redvaultsystems.redvaultmessage.data.api.PasswordRequest
import com.redvaultsystems.redvaultmessage.data.api.PlanStatusDto
import com.redvaultsystems.redvaultmessage.data.api.ReportRequest
import com.redvaultsystems.redvaultmessage.data.api.SettingsRequest
import com.redvaultsystems.redvaultmessage.data.api.UsernameRequest
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** A user-facing problem: `message` is safe to show as-is. */
open class UserFacingException(message: String) : Exception(message)

/**
 * A transfer stopped by the network (no connection, Tor down, the server
 * briefly unavailable), not refused: worth sending again later as it is.
 * The Tor outbox keeps the item and the Files tab pauses the upload.
 */
class TransferInterruptedException(message: String = "Network interrupted. Try again.") : UserFacingException(message)

/** Port of the account self-service half of ui/src/state/actions.ts, plus ui/src/lib/avatar.ts. */
class AccountRepository(
    private val api: ApiService,
    private val crypto: E2eCrypto,
    private val contentResolver: ContentResolver,
    private val recoveryKeys: RecoveryKey,
    private val signInKeys: SignInKeys,
) {
    suspend fun me(): MeResult = apiCall(ApiClient.json) { api.me() }

    suspend fun planStatus(): PlanStatusDto = apiCall(ApiClient.json) { api.planStatus() }

    suspend fun cancelPlan(): PlanStatusDto = apiCall(ApiClient.json) { api.cancelPlan() }

    suspend fun resumePlan(): PlanStatusDto = apiCall(ApiClient.json) { api.resumePlan() }

    suspend fun setAcceptMessageRequests(value: Boolean) {
        apiCall(ApiClient.json) { api.updateSettings(SettingsRequest(acceptMessageRequests = value)) }
    }

    suspend fun setTransparencyMode(value: Boolean) {
        apiCall(ApiClient.json) { api.updateSettings(SettingsRequest(transparencyMode = value)) }
    }

    suspend fun setShowOnlineStatus(value: Boolean) {
        apiCall(ApiClient.json) { api.updateSettings(SettingsRequest(showOnlineStatus = value)) }
    }

    /**
     * Re-wraps the (unchanged) private key under the new password on this
     * phone and sends it with the password change in one request; changing
     * the password alone would strand the old wrapped key. The server
     * revokes every other session; this one stays signed in.
     */
    suspend fun changePassword(identity: SelfIdentity, currentPassword: String, newPassword: String) = withContext(Dispatchers.Default) {
        val wrapped = crypto.wrapPrivateKey(identity.privateKey, newPassword)
        val next = signInKeys.newCredential(identity.handle, newPassword, wrapped.kdfSalt)
        val proof = signInKeys.passwordProof(identity.handle, currentPassword)
        apiCall(ApiClient.json) {
            api.changePassword(
                ChangePasswordRequest(
                    currentPassword = proof,
                    newPassword = next.password,
                    authVersion = next.authVersion,
                    kdfOps = wrapped.kdf?.ops,
                    kdfMem = wrapped.kdf?.mem,
                    encryptedPrivateKey = Base64Codec.encode(wrapped.encryptedPrivateKey),
                    privateKeyNonce = Base64Codec.encode(wrapped.nonce),
                    privateKeyKdfSalt = Base64Codec.encode(wrapped.kdfSalt),
                ),
            )
        }
        identity.wrappedKey = wrapped // the vault's password check follows the new password
    }

    /** Creates a new recovery key (replacing any old one); returns it formatted, to show once. Needs the password. */
    suspend fun createRecoveryKey(identity: SelfIdentity, currentPassword: String): String = withContext(Dispatchers.Default) {
        val key = recoveryKeys.generate()
        try {
            val m = recoveryKeys.materialFor(key, identity.privateKey)
            val proof = signInKeys.passwordProof(identity.handle, currentPassword)
            apiCall(ApiClient.json) {
                api.setRecoveryKey(SetRecoveryRequest(proof, m.recoveryAuth, m.recoveryEncryptedPrivateKey, m.recoveryPrivateKeyNonce))
            }
            RecoveryKey.format(key)
        } finally {
            key.fill(0)
        }
    }

    suspend fun changeUsername(username: String, previous: String) {
        apiCall(ApiClient.json) { api.changeUsername(UsernameRequest(username)) }
        signInKeys.movePin(previous, username)
    }

    /** Step 1 of 2: a pending secret that only takes effect once [confirmMfa] gets a valid code, so a bad scan can't lock anyone out. */
    suspend fun startMfaSetup(): MfaSetupResult = apiCall(ApiClient.json) { api.setupMfa() }

    suspend fun confirmMfa(code: String) {
        apiCall(ApiClient.json) { api.confirmMfa(MfaCodeRequest(code)) }
    }

    suspend fun disableMfa(identity: SelfIdentity, password: String) {
        val proof = signInKeys.passwordProof(identity.handle, password)
        apiCall(ApiClient.json) { api.disableMfa(PasswordRequest(proof)) }
    }

    /** Permanently deletes this account (password re-checked on the server) and signs out every device. */
    suspend fun deleteAccount(identity: SelfIdentity, password: String) {
        val proof = signInKeys.passwordProof(identity.handle, password)
        apiCall(ApiClient.json) { api.deleteAccount(PasswordRequest(proof)) }
    }

    /** Uploads a profile photo, which every user can see. Returns the new avatar URL. */
    suspend fun uploadAvatar(image: Uri): String? {
        val base64 = withContext(Dispatchers.IO) { AvatarJpeg.base64(contentResolver, image) }
        return apiCall(ApiClient.json) { api.uploadAvatar(AvatarUploadRequest(base64)) }.avatarUrl
    }

    suspend fun removeAvatar() {
        apiCall(ApiClient.json) { api.removeAvatar() }
    }

    suspend fun listBlocked(): List<BlockedUserDto> = apiCall(ApiClient.json) { api.listBlocked() }

    suspend fun block(userId: String) {
        apiCall(ApiClient.json) { api.block(BlockRequest(userId)) }
    }

    suspend fun unblock(userId: String) {
        apiCall(ApiClient.json) { api.unblock(userId) }
    }

    suspend fun report(userId: String, reason: String) {
        apiCall(ApiClient.json) { api.submitReport(ReportRequest(userId, reason.trim())) }
    }
}

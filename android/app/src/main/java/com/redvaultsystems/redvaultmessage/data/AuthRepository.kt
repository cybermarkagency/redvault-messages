package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.CryptoException
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.RecoveryKey
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.RecoveryCompleteRequest
import com.redvaultsystems.redvaultmessage.data.api.RecoveryDto
import com.redvaultsystems.redvaultmessage.data.api.RecoveryStartRequest
import com.redvaultsystems.redvaultmessage.crypto.WrappedPrivateKey
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.LoginRequest
import com.redvaultsystems.redvaultmessage.data.api.LoginResult
import com.redvaultsystems.redvaultmessage.data.api.MfaCodeRequest
import com.redvaultsystems.redvaultmessage.data.api.RegisterRequest
import com.redvaultsystems.redvaultmessage.data.api.SessionCookieJar
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.data.api.toLoginResult
import com.redvaultsystems.redvaultmessage.identity.LastHandleStore
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import com.redvaultsystems.redvaultmessage.push.PushRegistrar
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** A new account, and the recovery key to show once. */
class Registered(val identity: SelfIdentity, val recoveryKey: String)

/** What the person ticked on the sign-up form, sent as is: the server decides. */
data class SignupConsent(val termsAccepted: Boolean, val privacyAcknowledged: Boolean, val age18Confirmed: Boolean) {
    val complete: Boolean get() = termsAccepted && privacyAcknowledged && age18Confirmed
}

sealed interface RecoverOutcome {
    data class SignedIn(val identity: SelfIdentity) : RecoverOutcome

    /** The account has two-factor on: ask for the code and try again. Nothing changed yet. */
    data object CodeRequired : RecoverOutcome
}

sealed interface LoginOutcome {
    data class SignedIn(val identity: SelfIdentity) : LoginOutcome

    /** Password was right, but the account has MFA: call [AuthRepository.verifyMfa] next. */
    data object MfaRequired : LoginOutcome
}

/** The account's wrapped private key and the settings it needs, which [E2eCrypto.unwrapPrivateKey] checks before using them. */
internal fun wrappedKeyOf(result: LoginResult) = WrappedPrivateKey(
    encryptedPrivateKey = Base64Codec.decode(result.encryptedPrivateKey),
    nonce = Base64Codec.decode(result.privateKeyNonce),
    kdfSalt = Base64Codec.decode(result.privateKeyKdfSalt),
    kdf = kdfSettings(result.privateKeyKdfOps, result.privateKeyKdfMem),
)

/**
 * Port of the auth half of ui/src/state/actions.ts. Key generation, Argon2id
 * and unwrapping run on Dispatchers.Default: Argon2id alone takes a
 * noticeable fraction of a second and would freeze the UI on the main thread.
 */
class AuthRepository(
    private val api: ApiService,
    private val crypto: E2eCrypto,
    private val cookieJar: SessionCookieJar,
    private val lastHandleStore: LastHandleStore,
    private val pushRegistrar: PushRegistrar,
    private val recoveryKeys: RecoveryKey,
    private val signInKeys: SignInKeys = SignInKeys(api, crypto, MemorySignInPins()),
) {
    val lastHandle: String? get() = lastHandleStore.load()

    suspend fun register(handle: String, password: String, consent: SignupConsent): Registered = withContext(Dispatchers.Default) {
        val keyPair = crypto.generateKeyPair()
        val wrapped = crypto.wrapPrivateKey(keyPair.privateKey, password)
        // Every account gets a recovery key, shown once right after sign-up.
        val recoveryKey = recoveryKeys.generate()
        val recovery = recoveryKeys.materialFor(recoveryKey, keyPair.privateKey)
        val credential = signInKeys.newCredential(handle, password, wrapped.kdfSalt)
        val result = apiCall(ApiClient.json) {
            api.register(
                RegisterRequest(
                    username = handle,
                    password = credential.password,
                    authVersion = credential.authVersion,
                    kdfOps = wrapped.kdf?.ops,
                    kdfMem = wrapped.kdf?.mem,
                    publicKey = Base64Codec.encode(keyPair.publicKey),
                    encryptedPrivateKey = Base64Codec.encode(wrapped.encryptedPrivateKey),
                    privateKeyNonce = Base64Codec.encode(wrapped.nonce),
                    privateKeyKdfSalt = Base64Codec.encode(wrapped.kdfSalt),
                    recovery = RecoveryDto(recovery.recoveryAuth, recovery.recoveryEncryptedPrivateKey, recovery.recoveryPrivateKeyNonce),
                    termsAccepted = consent.termsAccepted,
                    privacyAcknowledged = consent.privacyAcknowledged,
                    age18Confirmed = consent.age18Confirmed,
                ),
            )
        }
        if (credential.authVersion != null) signInKeys.pin(handle)
        lastHandleStore.save(handle)
        val formattedKey = RecoveryKey.format(recoveryKey)
        recoveryKey.fill(0)
        val identity = SelfIdentity(
            handle = handle,
            userId = result.userId,
            publicKey = keyPair.publicKey,
            privateKey = keyPair.privateKey,
            avatarUrl = result.avatarUrl,
            role = result.role,
            subscriptionTier = result.subscriptionTier,
            mfaEnabled = false, // freshly registered accounts never start with MFA on
            wrappedKey = wrapped,
        )
        Registered(identity, formattedKey)
    }

    /**
     * Recovers an account with its recovery key: unwraps the private key on
     * this phone (checking it matches the account), re-wraps it under the
     * new password, and signs in. Every other session is signed out.
     */
    suspend fun recover(handle: String, recoveryKeyText: String, newPassword: String, code: String?): RecoverOutcome =
        withContext(Dispatchers.Default) {
            val key = RecoveryKey.parse(recoveryKeyText)
                ?: throw UserFacingException("That doesn't look like a recovery key. Check it and try again.")
            try {
                val auth = recoveryKeys.authFor(key)
                val started = try {
                    apiCall(ApiClient.json) { api.recoveryStart(RecoveryStartRequest(handle, auth)) }
                } catch (e: ApiException) {
                    throw UserFacingException(
                        when (e.code) {
                            "account_locked" -> "Too many tries. Wait a while and try again."
                            "network_error" -> e.message ?: "Can't reach the server."
                            else -> "That handle and recovery key don't match."
                        },
                    )
                }
                if (started.mfaRequired && code.isNullOrBlank()) return@withContext RecoverOutcome.CodeRequired
                val privateKey = try {
                    recoveryKeys.unwrap(key, started.recoveryEncryptedPrivateKey, started.recoveryPrivateKeyNonce, started.publicKey)
                } catch (e: CryptoException) {
                    throw UserFacingException("That recovery key doesn't unlock this account.")
                }
                val wrapped = crypto.wrapPrivateKey(privateKey, newPassword)
                val next = signInKeys.newCredential(handle, newPassword, wrapped.kdfSalt)
                val result = try {
                    apiCall(ApiClient.json) {
                        api.recoveryComplete(
                            RecoveryCompleteRequest(
                                username = handle,
                                recoveryAuth = auth,
                                code = code?.takeIf { started.mfaRequired },
                                newPassword = next.password,
                                authVersion = next.authVersion,
                                kdfOps = wrapped.kdf?.ops,
                                kdfMem = wrapped.kdf?.mem,
                                encryptedPrivateKey = Base64Codec.encode(wrapped.encryptedPrivateKey),
                                privateKeyNonce = Base64Codec.encode(wrapped.nonce),
                                privateKeyKdfSalt = Base64Codec.encode(wrapped.kdfSalt),
                            ),
                        )
                    }.toLoginResult()
                } catch (e: ApiException) {
                    throw UserFacingException(if (e.code == "invalid_code") "Wrong code. Check your authenticator app and try again." else e.message ?: "Couldn't recover the account.")
                }
                if (next.authVersion != null) signInKeys.pin(handle)
                lastHandleStore.save(handle)
                RecoverOutcome.SignedIn(
                    SelfIdentity(
                        handle = handle,
                        userId = result.userId,
                        publicKey = Base64Codec.decode(result.publicKey),
                        privateKey = privateKey,
                        avatarUrl = result.avatarUrl,
                        role = result.role,
                        subscriptionTier = result.subscriptionTier,
                        mfaEnabled = started.mfaRequired,
                        wrappedKey = wrapped,
                    ),
                )
            } finally {
                key.fill(0)
            }
        }

    suspend fun login(handle: String, password: String): LoginOutcome = withContext(Dispatchers.Default) {
        val credential = signInKeys.credentialFor(handle, password)
        val response = apiCall(ApiClient.json) {
            api.login(LoginRequest(handle, credential.password, credential.authVersion, credential.upgradeAuthKey))
        }
        if (credential.authVersion != null || credential.upgradeAuthKey != null) signInKeys.pin(handle)
        if (response.mfaRequired) {
            LoginOutcome.MfaRequired
        } else {
            LoginOutcome.SignedIn(finishLogin(handle, password, response.toLoginResult(), mfaEnabled = false))
        }
    }

    /** Completes a login that returned [LoginOutcome.MfaRequired], with the same handle and password. */
    suspend fun verifyMfa(handle: String, password: String, code: String): SelfIdentity = withContext(Dispatchers.Default) {
        val response = apiCall(ApiClient.json) { api.verifyMfa(MfaCodeRequest(code)) }
        finishLogin(handle, password, response.toLoginResult(), mfaEnabled = true)
    }

    /** Best-effort server logout; local state is cleared either way. */
    suspend fun logout(identity: SelfIdentity?) {
        try {
            // First, while the session is still valid, so the server stops
            // waking this phone for an account that's no longer on it.
            pushRegistrar.unregister()
            apiCall(ApiClient.json) { api.logout() }
        } catch (_: Exception) {
            // The client forgets its keys and session regardless.
        } finally {
            cookieJar.clear()
            identity?.wipe()
        }
    }

    /** Unwraps the private key locally with the just-entered password. The server never sees it unwrapped. */
    private fun finishLogin(handle: String, password: String, result: LoginResult, mfaEnabled: Boolean): SelfIdentity {
        val wrapped = wrappedKeyOf(result)
        val privateKey = crypto.unwrapPrivateKey(wrapped, password)
        // Other people's keys get sealed to this one, and copies of our own: it has to be ours.
        if (!crypto.publicKeyOf(privateKey).contentEquals(Base64Codec.decode(result.publicKey))) {
            privateKey.fill(0)
            throw UserFacingException("This account's keys don't match. Nothing was opened.")
        }
        lastHandleStore.save(handle)
        return SelfIdentity(
            handle = handle,
            userId = result.userId,
            publicKey = Base64Codec.decode(result.publicKey),
            privateKey = privateKey,
            avatarUrl = result.avatarUrl,
            role = result.role,
            subscriptionTier = result.subscriptionTier,
            mfaEnabled = mfaEnabled,
            wrappedKey = wrapped,
        )
    }
}

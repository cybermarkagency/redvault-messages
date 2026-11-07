package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.Serializable
import retrofit2.http.Body
import retrofit2.http.DELETE
import retrofit2.http.GET
import retrofit2.http.PUT
import retrofit2.http.Path
import retrofit2.http.Query

/**
 * Settings' security pages: linked devices (server/src/routes/authRoutes.ts)
 * and the public transparency log (transparencyRoutes.ts, mounted under
 * /api/public).
 */
interface SecurityApi {
    @GET("auth/sessions")
    suspend fun sessions(): List<SessionDto>

    @PUT("auth/sessions/current/label")
    suspend fun setLabel(@Body label: SealedLabelDto)

    @DELETE("auth/sessions/{id}")
    suspend fun signOut(@Path("id") id: String)

    /** Signs out every device but this one. */
    @DELETE("auth/sessions")
    suspend fun signOutOthers()

    @GET("public/transparency-log/checkpoint")
    suspend fun checkpoint(): CheckpointResponse

    @GET("public/transparency-log/entries")
    suspend fun entries(@Query("limit") limit: Int = 50, @Query("q") q: String? = null, @Query("before") before: Long? = null): List<LogEntryDto>

    @GET("public/transparency-log/entries/{index}/proof")
    suspend fun proof(@Path("index") index: Long, @Query("treeSize") treeSize: Long): InclusionProofDto

    @GET("public/transparency-log/consistency")
    suspend fun consistency(@Query("from") from: Long, @Query("to") to: Long): ConsistencyProofDto
}

@Serializable
data class SealedLabelDto(val ciphertext: String, val nonce: String)

@Serializable
data class SessionDto(
    val id: String,
    val current: Boolean,
    /** Password entered, two-factor code not (yet). */
    val awaitingTwoFactor: Boolean = false,
    val expiresOn: String,
    /** The device's name, encrypted by your own devices. */
    val label: SealedLabelDto? = null,
)

@Serializable
data class CheckpointDto(val treeSize: Long, val rootHash: String, val signedAt: String, val signature: String, val publicKey: String)

@Serializable
data class CheckpointResponse(val checkpoint: CheckpointDto? = null)

@Serializable
data class LogEntryDto(val index: Long, val commitment: String, val sizeKb: Double = 0.0, val checkpointSize: Long = 0, val loggedAt: String = "")

@Serializable
data class InclusionProofDto(val index: Long, val treeSize: Long, val leafHash: String, val path: List<String> = emptyList())

@Serializable
data class ConsistencyProofDto(val from: Long, val to: Long, val path: List<String> = emptyList())

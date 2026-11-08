// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject
import okhttp3.ResponseBody
import retrofit2.http.Body
import retrofit2.http.DELETE
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.PUT
import retrofit2.http.Path

/**
 * Encrypted cloud storage ("Files": server/src/routes/backupRoutes.ts) and
 * the password manager (server/src/routes/vaultRoutes.ts). Only ciphertext,
 * sizes and hashes cross these calls; everything readable is sealed on the
 * phone (crypto/DriveCrypto.kt, crypto/VaultCrypto.kt).
 */
interface DriveApi {
    @GET("backup/usage")
    suspend fun usage(): DriveUsageDto

    @GET("backup/items")
    suspend fun list(): DriveListingDto

    @GET("backup/shared-with-me")
    suspend fun sharedWithMe(): DriveSharedListDto

    @POST("backup/folders")
    suspend fun createFolder(@Body body: DriveMetaRequest): DriveEntryDto

    @PUT("backup/{id}/meta")
    suspend fun updateMeta(@Path("id") id: String, @Body body: DriveMetaRequest): DriveEntryDto

    /** 413 "backup_storage_full" (worded for the person) when it doesn't fit the plan. */
    @POST("backup/upload-request")
    suspend fun requestUpload(@Body body: DriveUploadRequest): DriveUploadAuthDto

    /** A resumable upload in parts (db/migrations/034): the answer's upload has mode "parts". */
    @POST("backup/upload-request")
    suspend fun requestUploadParts(@Body body: DriveUploadPartsRequest): DrivePartsAuthDto

    /** The parts the server has of a resumable upload. */
    @GET("backup/{id}/parts")
    suspend fun listParts(@Path("id") id: String): DrivePartsDto

    @POST("backup/{id}/complete")
    suspend fun completeUpload(@Path("id") id: String, @Body body: JsonObject)

    @GET("backup/{id}/download")
    suspend fun download(@Path("id") id: String): DriveDownloadDto

    @DELETE("backup/{id}")
    suspend fun remove(@Path("id") id: String)

    @POST("backup/{id}/shares")
    suspend fun share(@Path("id") id: String, @Body body: DriveShareRequest)

    @DELETE("backup/{id}/shares/{recipientId}")
    suspend fun unshare(@Path("id") id: String, @Path("recipientId") recipientId: String)

    @POST("backup/{id}/links")
    suspend fun createLink(@Path("id") id: String, @Body body: DriveLinkRequest): DriveLinkDto

    @GET("backup/links")
    suspend fun links(): DriveLinksDto

    @DELETE("backup/links/{linkId}")
    suspend fun deleteLink(@Path("linkId") linkId: String)

    // --- Password vault ---

    @GET("vault/items")
    suspend fun vaultItems(): VaultItemsDto

    @POST("vault/items")
    suspend fun createVaultItem(@Body body: VaultItemRequest): VaultItemDto

    /** 409 "vault_conflict" when another device changed it since [VaultItemRequest.revision]. */
    @PUT("vault/items/{id}")
    suspend fun updateVaultItem(@Path("id") id: String, @Body body: VaultItemRequest): VaultItemDto

    @DELETE("vault/items/{id}")
    suspend fun deleteVaultItem(@Path("id") id: String)

    @GET("vault/shares")
    suspend fun vaultShares(): VaultSharesDto

    @GET("vault/shared-with-me")
    suspend fun vaultSharedWithMe(): VaultSharedItemsDto

    @PUT("vault/items/{id}/shares/{recipientId}")
    suspend fun shareVaultItem(@Path("id") id: String, @Path("recipientId") recipientId: String, @Body body: VaultShareRequest)

    @DELETE("vault/items/{id}/shares/{recipientId}")
    suspend fun unshareVaultItem(@Path("id") id: String, @Path("recipientId") recipientId: String)

    /** "SUFFIX:COUNT" lines for a 5-character SHA-1 prefix. */
    @GET("vault/breach-range/{prefix}")
    suspend fun breachRange(@Path("prefix") prefix: String): ResponseBody
}

@Serializable
data class DriveUsageDto(val used: Long, val limit: Long, val remaining: Long, val tier: String)

@Serializable
data class DriveEntryDto(
    val id: String,
    val kind: String = "file",
    val encryptedSize: Long = 0,
    val ciphertextSha256: String? = null,
    val metaCiphertext: String,
    val metaNonce: String,
    val sharedWith: List<String> = emptyList(),
)

@Serializable
data class DriveListingDto(val items: List<DriveEntryDto>, val usage: DriveUsageDto)

@Serializable
data class DriveSharedEntryDto(
    val id: String,
    val ownerId: String,
    val encryptedSize: Long = 0,
    val ciphertextSha256: String? = null,
    val sealedMeta: String,
)

@Serializable
data class DriveSharedListDto(val items: List<DriveSharedEntryDto>)

@Serializable
data class DriveMetaRequest(val metaCiphertext: String, val metaNonce: String)

@Serializable
data class DriveUploadRequest(val encryptedSize: Long, val ciphertextSha256: String, val metaCiphertext: String, val metaNonce: String)

@Serializable
data class DriveUploadAuthDto(val objectId: String, val upload: TransferTargetDto)

/** [partsSha256]: SHA-256 over every part's raw digest, in order (lowercase hex). */
@Serializable
data class DriveUploadPartsRequest(
    val encryptedSize: Long,
    val ciphertextSha256: String,
    val metaCiphertext: String,
    val metaNonce: String,
    val partSize: Long,
    val partsSha256: String,
)

@Serializable
data class DrivePartsTargetDto(val mode: String, val url: String, val method: String, val partSize: Long = 0, val partCount: Int = 0)

@Serializable
data class DrivePartsAuthDto(val objectId: String, val upload: DrivePartsTargetDto)

@Serializable
data class DriveReceivedPartDto(val partNumber: Int, val size: Long, val sha256: String)

@Serializable
data class DrivePartsDto(val received: List<DriveReceivedPartDto> = emptyList())

@Serializable
data class DriveDownloadDto(val download: TransferTargetDto)

@Serializable
data class DriveShareRequest(val recipientId: String, val sealedMeta: String)

@Serializable
data class DriveLinkRequest(
    val sealedMeta: String,
    val metaNonce: String,
    val passwordSalt: String?,
    val allowDownload: Boolean,
    val expiresInDays: Int?,
)

@Serializable
data class DriveLinkDto(
    val id: String,
    val backupId: String,
    val sealedMeta: String,
    val metaNonce: String,
    val hasPassword: Boolean,
    val allowDownload: Boolean,
    val expiresAt: String? = null,
)

@Serializable
data class DriveLinksDto(val links: List<DriveLinkDto>)

@Serializable
data class VaultItemDto(val id: String, val ciphertext: String, val nonce: String, val revision: Int)

@Serializable
data class VaultItemsDto(val items: List<VaultItemDto>)

@Serializable
data class VaultItemRequest(val ciphertext: String, val nonce: String, val revision: Int? = null)

@Serializable
data class VaultShareDto(val itemId: String, val recipientId: String)

@Serializable
data class VaultSharesDto(val shares: List<VaultShareDto>)

@Serializable
data class VaultSharedItemDto(val itemId: String, val ownerId: String, val sealed: String)

@Serializable
data class VaultSharedItemsDto(val items: List<VaultSharedItemDto>)

@Serializable
data class VaultShareRequest(val sealed: String)

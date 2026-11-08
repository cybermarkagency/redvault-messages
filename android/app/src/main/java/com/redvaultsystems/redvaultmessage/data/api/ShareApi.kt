// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject
import retrofit2.http.Body
import retrofit2.http.DELETE
import retrofit2.http.GET
import retrofit2.http.PATCH
import retrofit2.http.POST
import retrofit2.http.PUT
import retrofit2.http.Path

/**
 * Public share links for Files, the owner's side (server/src/routes/shareRoutes.ts,
 * docs/public-share-links.md). Only verifiers, ciphertext and limits cross
 * these calls; the link's secret, the share key and every name are sealed on
 * the phone (crypto/ShareCrypto.kt). Request bodies are JSON objects built
 * by ShareRepository so that "no limit" (null) is sent as null, not left out.
 */
interface ShareApi {
    @GET("shares")
    suspend fun list(): ShareLinksDto

    @POST("shares")
    suspend fun create(@Body body: JsonObject): ShareLinkDto

    @PATCH("shares/{id}")
    suspend fun update(@Path("id") id: String, @Body body: JsonObject): ShareLinkDto

    @PUT("shares/{id}/password")
    suspend fun setPassword(@Path("id") id: String, @Body body: JsonObject): ShareLinkDto

    @POST("shares/{id}/regenerate")
    suspend fun regenerate(@Path("id") id: String, @Body body: JsonObject): ShareLinkDto

    @POST("shares/{id}/revoke")
    suspend fun revoke(@Path("id") id: String, @Body body: JsonObject): ShareLinkDto

    @DELETE("shares/{id}")
    suspend fun remove(@Path("id") id: String)

    @GET("shares/{id}/items")
    suspend fun items(@Path("id") id: String): ShareItemsDto

    @POST("shares/{id}/items")
    suspend fun upsertItems(@Path("id") id: String, @Body body: ShareItemsRequest): ShareStoredDto

    @POST("shares/{id}/items/remove")
    suspend fun removeItems(@Path("id") id: String, @Body body: ShareRemoveItemsRequest): ShareRemovedDto

    @GET("shares/incoming")
    suspend fun incoming(): ShareIncomingListDto

    @POST("shares/incoming/{id}/accept")
    suspend fun accept(@Path("id") id: String, @Body body: DriveMetaRequest): ShareAcceptedDto

    @POST("shares/incoming/{id}/reject")
    suspend fun reject(@Path("id") id: String, @Body body: JsonObject)
}

@Serializable
data class SharePasswordDto(val salt: String, val ops: Long, val mem: Long)

@Serializable
data class ShareUploadLimitsDto(
    val maxFileSize: Long? = null,
    val allowance: Long? = null,
    val maxFiles: Long? = null,
    val count: Long = 0,
    val bytes: Long = 0,
    val inProgress: Long = 0,
)

@Serializable
data class ShareLinkDto(
    val id: String,
    val publicId: String,
    val backupId: String,
    val kind: String,
    val permission: String,
    val keyEpoch: Int = 1,
    val password: SharePasswordDto? = null,
    val envelope: String,
    val envelopeNonce: String,
    val ownerBox: String,
    val ownerBoxNonce: String,
    val uploadKeySalt: String,
    val expiresAt: String? = null,
    val expired: Boolean = false,
    val revoked: Boolean = false,
    val maxDownloads: Long? = null,
    val downloadCount: Long = 0,
    val openCount: Long = 0,
    val upload: ShareUploadLimitsDto = ShareUploadLimitsDto(),
    val incoming: Int = 0,
)

@Serializable
data class ShareLinksDto(val links: List<ShareLinkDto>, val incoming: Int = 0)

@Serializable
data class ShareOwnerItemDto(val itemId: String, val backupId: String, val kind: String = "file", val metaCiphertext: String, val metaNonce: String)

@Serializable
data class ShareItemsDto(val keyEpoch: Int, val items: List<ShareOwnerItemDto>)

@Serializable
data class ShareItemInput(val itemId: String, val backupId: String, val metaCiphertext: String, val metaNonce: String)

@Serializable
data class ShareItemsRequest(val keyEpoch: Int, val items: List<ShareItemInput>)

@Serializable
data class ShareRemoveItemsRequest(val itemIds: List<String>)

@Serializable
data class ShareStoredDto(val stored: Int)

@Serializable
data class ShareRemovedDto(val removed: Int)

@Serializable
data class ShareIncomingDto(
    val id: String,
    val shareId: String? = null,
    val encryptedSize: Long,
    val ciphertextSha256: String? = null,
    val sealedMeta: String? = null,
    val keySalt: String,
)

@Serializable
data class ShareIncomingListDto(val items: List<ShareIncomingDto>)

@Serializable
data class ShareAcceptedDto(val item: DriveEntryDto)

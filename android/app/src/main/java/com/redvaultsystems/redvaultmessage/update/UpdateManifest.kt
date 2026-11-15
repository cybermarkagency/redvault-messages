// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.update

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import java.time.Instant
import java.util.Base64

/**
 * The Android app's signed update manifest (android-latest.json), the same format and release key as the desktop's
 * latest.json (desktop/core/src/update/update.cpp):
 *
 *     {"payload": "<base64 of the release JSON>", "signature": "<base64 Ed25519 signature of those bytes>"}
 *
 * Only the holder of the offline release key can sign one, and the app trusts nothing it can't verify against the key
 * built in here: not the server, not the network. It only moves forward (a higher versionCode than this build), only
 * downloads from the expected folder, and only installs an APK whose size and SHA-256 match. Android itself also
 * refuses an APK not signed with this app's own signing key.
 */
object UpdateManifest {
    const val PRODUCT = "redvault-android"
    /** The published (website) folder the signed APK URL must be in; over Tor the onion service's copy is used. */
    const val DOWNLOAD_PREFIX = "https://www.redvaultsystems.com/redvaultmessages/desktop/"
    const val MANIFEST_NAME = "android-latest.json"
    /** The release key's public half, the same as the desktop's (desktop/app/src/update_key.hpp). */
    const val PUBLIC_KEY_B64 = "4GOi/rqNFDftNAHc8vCPKL7cukGzVAVXHLUD2ckyWi0="

    data class Release(
        val version: String,
        val versionCode: Long,
        val url: String,
        val sha256: String,
        val size: Long,
        val notes: String,
    )

    /** [release]: newer than this build, or null. [stale]: the manifest's signed expiry has passed (a newer one may exist). */
    data class Check(val release: Release?, val stale: Boolean)

    class Invalid(message: String) : Exception(message)

    /**
     * Verifies [manifestJson] with [publicKey] through [verifyEd25519] (libsodium's crypto_sign_verify_detached), and
     * returns the release if it's newer than [currentVersionCode]. Throws [Invalid] if anything doesn't check out.
     */
    fun check(
        manifestJson: String,
        publicKey: ByteArray,
        currentVersionCode: Long,
        nowMillis: Long,
        verifyEd25519: (signature: ByteArray, message: ByteArray, publicKey: ByteArray) -> Boolean,
    ): Check {
        if (publicKey.size != 32) throw Invalid("No valid update key in this build")
        val envelope = runCatching { Json.parseToJsonElement(manifestJson).jsonObject }.getOrElse { throw Invalid("The update manifest isn't valid JSON") }
        val payload = decode(envelope, "payload")
        val signature = decode(envelope, "signature")
        // Nothing from the payload is looked at before the signature checks out.
        if (signature.size != 64 || !verifyEd25519(signature, payload, publicKey)) throw Invalid("The update manifest's signature is invalid")
        val body = runCatching { Json.parseToJsonElement(String(payload, Charsets.UTF_8)).jsonObject }
            .getOrElse { throw Invalid("The signed update isn't valid JSON") }
        if (string(body, "product") != PRODUCT) throw Invalid("The update manifest is for a different app")
        val versionCode = body["versionCode"]?.jsonPrimitive?.longOrNull ?: throw Invalid("The update manifest has no versionCode")
        val size = body["size"]?.jsonPrimitive?.longOrNull ?: throw Invalid("The update manifest has no size")
        val release = Release(
            version = string(body, "version"),
            versionCode = versionCode,
            url = string(body, "url"),
            sha256 = string(body, "sha256").lowercase(),
            size = size,
            notes = body["notes"]?.jsonPrimitive?.contentOrNull.orEmpty(),
        )
        if (!Regex("""^\d+\.\d+\.\d+$""").matches(release.version)) throw Invalid("The update has a malformed version")
        if (versionCode <= 0 || size <= 0) throw Invalid("The update has a malformed versionCode or size")
        if (!release.url.startsWith(DOWNLOAD_PREFIX) || !release.url.endsWith(".apk") || release.url.substring(DOWNLOAD_PREFIX.length).contains('/')) {
            throw Invalid("The update's APK isn't in the download folder")
        }
        if (!Regex("^[0-9a-f]{64}$").matches(release.sha256)) throw Invalid("The update has a malformed sha256")
        val stale = body["expires"]?.jsonPrimitive?.contentOrNull?.let { expires ->
            val at = runCatching { Instant.parse(expires).toEpochMilli() }.getOrElse { throw Invalid("The update manifest has an invalid expiry") }
            nowMillis >= at
        } ?: true
        return Check(release.takeIf { it.versionCode > currentVersionCode }, stale)
    }

    /** The onion service's (or a test server's) copy of the published folder: the manifest URL there. */
    fun manifestUrl(apiOrigin: String): String = "$apiOrigin/redvaultmessages/desktop/$MANIFEST_NAME"

    /** Where to download [release] from: its signed URL moved into [apiOrigin]'s copy of the folder. */
    fun apkUrl(release: Release, apiOrigin: String): String = "$apiOrigin/redvaultmessages/desktop/" + release.url.substring(DOWNLOAD_PREFIX.length)

    private fun decode(envelope: JsonObject, name: String): ByteArray =
        runCatching { Base64.getDecoder().decode(string(envelope, name)) }.getOrElse { throw Invalid("The update manifest isn't valid base64") }

    private fun string(body: JsonObject, name: String): String =
        body[name]?.jsonPrimitive?.contentOrNull ?: throw Invalid("The update manifest is missing $name")
}

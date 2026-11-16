// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.update

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant
import java.util.Base64

/**
 * The Android update manifest (update/UpdateManifest.kt). The vector in android_update_vector.json is signed by the
 * release tool's own code (desktop/tests/update_test.cpp) with a public test seed, so the signer and this checker
 * are tested against each other.
 */
class UpdateManifestTest {
    private val sodium = LazySodiumJava(SodiumJava())
    private val verify: (ByteArray, ByteArray, ByteArray) -> Boolean =
        { signature, message, key -> sodium.cryptoSignVerifyDetached(signature, message, message.size, key) }
    private val vector = Json.parseToJsonElement(javaClass.classLoader!!.getResource("android_update_vector.json")!!.readText()).jsonObject
    private val manifest = vector["manifest"]!!.jsonPrimitive.content
    private val key = Base64.getDecoder().decode(vector["publicKey"]!!.jsonPrimitive.content)
    private val now = Instant.parse("2026-11-13T12:00:00Z").toEpochMilli()

    /** Signs [payload] with a throwaway key, for manifests the release tool would never write. */
    private fun forge(payload: String): Pair<String, ByteArray> {
        val publicKey = ByteArray(32)
        val secretKey = ByteArray(64)
        sodium.cryptoSignKeypair(publicKey, secretKey)
        val bytes = payload.toByteArray()
        val signature = ByteArray(64)
        sodium.cryptoSignDetached(signature, bytes, bytes.size.toLong(), secretKey)
        val e = Base64.getEncoder()
        return """{"payload":"${e.encodeToString(bytes)}","signature":"${e.encodeToString(signature)}"}""" to publicKey
    }

    private fun payload(
        product: String = "redvault-android",
        url: String = "https://www.redvaultsystems.com/redvaultmessages/desktop/RedVault-Messages-0.6.2.apk",
        versionCode: String = "12",
        expires: String = "2027-02-11T00:00:00Z",
    ) = """{"product":"$product","version":"0.6.2","url":"$url","sha256":"${"c".repeat(64)}","size":10,"notes":"","published":"2026-11-13T00:00:00Z","expires":"$expires","versionCode":$versionCode}"""

    @Test fun theReleaseToolsManifestIsAcceptedWhenNewer() {
        val check = UpdateManifest.check(manifest, key, currentVersionCode = 11, nowMillis = now, verifyEd25519 = verify)
        val release = check.release!!
        assertEquals("0.6.2", release.version)
        assertEquals(12L, release.versionCode)
        assertEquals(4321L, release.size)
        assertEquals("https://www.redvaultsystems.com/redvaultmessages/desktop/RedVault-Messages-0.6.2.apk", release.url)
        assertFalse(check.stale)
    }

    @Test fun theSameOrAnOlderVersionIsNeverOffered() {
        assertNull(UpdateManifest.check(manifest, key, 12, now, verify).release)
        assertNull(UpdateManifest.check(manifest, key, 40, now, verify).release)
    }

    @Test fun anotherKeyOrATamperedManifestIsRefused() {
        val otherKey = key.copyOf().also { it[0] = (it[0] + 1).toByte() }
        assertThrows(UpdateManifest.Invalid::class.java) { UpdateManifest.check(manifest, otherKey, 11, now, verify) }
        val tampered = manifest.replace("eyJwcm9kdWN0Ijoi", "eyJwcm9kdWN0Ijpi")
        assertThrows(UpdateManifest.Invalid::class.java) { UpdateManifest.check(tampered, key, 11, now, verify) }
        assertThrows(UpdateManifest.Invalid::class.java) { UpdateManifest.check("not json", key, 11, now, verify) }
    }

    @Test fun aDesktopManifestIsNotAnAndroidUpdate() {
        val (signed, publicKey) = forge(payload(product = "redvault-desktop"))
        assertThrows(UpdateManifest.Invalid::class.java) { UpdateManifest.check(signed, publicKey, 1, now, verify) }
    }

    @Test fun anApkOutsideTheDownloadFolderIsRefused() {
        for (url in listOf(
            "https://evil.example/RedVault-Messages-0.6.2.apk",
            "https://www.redvaultsystems.com/redvaultmessages/desktop/sub/RedVault-Messages-0.6.2.apk",
            "https://www.redvaultsystems.com/redvaultmessages/desktop/RedVault-Messages-0.6.2.exe",
        )) {
            val (signed, publicKey) = forge(payload(url = url))
            assertThrows(url, UpdateManifest.Invalid::class.java) { UpdateManifest.check(signed, publicKey, 1, now, verify) }
        }
    }

    @Test fun aManifestWithoutAVersionCodeIsRefused() {
        val (signed, publicKey) = forge(payload(versionCode = "0"))
        assertThrows(UpdateManifest.Invalid::class.java) { UpdateManifest.check(signed, publicKey, 1, now, verify) }
    }

    @Test fun anExpiredManifestIsStillReadButMarkedStale() {
        val (signed, publicKey) = forge(payload(expires = "2026-01-01T00:00:00Z"))
        val check = UpdateManifest.check(signed, publicKey, 1, now, verify)
        assertTrue(check.stale)
        assertEquals(12L, check.release!!.versionCode)
    }

    @Test fun theApkIsFetchedFromTheServersCopyOfTheFolder() {
        val release = UpdateManifest.check(manifest, key, 11, now, verify).release!!
        val onion = "http://abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz2345.onion"
        assertEquals("$onion/redvaultmessages/desktop/RedVault-Messages-0.6.2.apk", UpdateManifest.apkUrl(release, onion))
        assertEquals("$onion/redvaultmessages/desktop/android-latest.json", UpdateManifest.manifestUrl(onion))
    }

    @Test fun onTheBackupRouteTheWebsitesPublishedFolderIsUsedAsSigned() {
        val release = UpdateManifest.check(manifest, key, 11, now, verify).release!!
        val website = UpdateManifest.publishedOrigin()
        assertEquals("https://www.redvaultsystems.com", website)
        // The APK's URL there is exactly the signed one; the manifest sits in the same folder.
        assertEquals(release.url, UpdateManifest.apkUrl(release, website))
        assertEquals(UpdateManifest.DOWNLOAD_PREFIX + UpdateManifest.MANIFEST_NAME, UpdateManifest.manifestUrl(website))
    }

    @Test fun theBuiltInKeyIsTheDesktopsReleaseKey() {
        assertEquals(32, Base64.getDecoder().decode(UpdateManifest.PUBLIC_KEY_B64).size)
        val desktop = java.io.File("../../desktop/app/src/update_key.hpp").readText()
        assertTrue(desktop.contains("\"${UpdateManifest.PUBLIC_KEY_B64}\""))
    }
}

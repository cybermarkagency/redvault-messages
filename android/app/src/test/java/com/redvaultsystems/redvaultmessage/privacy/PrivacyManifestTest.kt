// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.privacy

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * Proof that the app has no way to collect location or usage data: it
 * can't hold a location, advertising-ID, phone-identity or contacts
 * permission, there's no Firebase or Google Play services at all, no analytics
 * or crash-reporting library is a dependency, and no shipped source file calls
 * a location, device-fingerprint or tracking API. Unit tests run with the
 * app module (android/app) as the working directory.
 */
class PrivacyManifestTest {
    private val manifest = File("src/main/AndroidManifest.xml").readText()
    private val sources = File("src/main/java").walk().filter { it.extension == "kt" }.map { it to it.readText() }.toList()

    @Test
    fun scansTheRealSources() = assertTrue(sources.size > 30)

    @Test
    fun locationAdvertisingPhoneAndContactPermissionsAreStripped() {
        for (permission in listOf(
            "android.permission.ACCESS_FINE_LOCATION",
            "android.permission.ACCESS_COARSE_LOCATION",
            "android.permission.ACCESS_BACKGROUND_LOCATION",
            "com.google.android.gms.permission.AD_ID",
            "android.permission.ACCESS_ADSERVICES_AD_ID",
            "android.permission.READ_PHONE_STATE",
            "android.permission.READ_CONTACTS",
        )) {
            assertTrue("$permission must be removed from the merged manifest",
                manifest.contains("""<uses-permission android:name="$permission" tools:node="remove" />"""))
        }
        val requested = Regex("""<uses-permission android:name="([^"]+)" />""").findAll(manifest).map { it.groupValues[1] }.toSet()
        assertEquals(
            setOf(
                "android.permission.INTERNET",
                "android.permission.POST_NOTIFICATIONS",
                "android.permission.RECORD_AUDIO",
                // The password vault's fingerprint / face unlock.
                "android.permission.USE_BIOMETRIC",
                // Calls: video, voice routing, ringing, and keeping a call going in the background.
                "android.permission.CAMERA",
                "android.permission.MODIFY_AUDIO_SETTINGS",
                "android.permission.VIBRATE",
                "android.permission.FOREGROUND_SERVICE",
                "android.permission.FOREGROUND_SERVICE_MICROPHONE",
                "android.permission.FOREGROUND_SERVICE_CAMERA",
                "android.permission.FOREGROUND_SERVICE_MEDIA_PROJECTION",
                // The background connection for notifications when no UnifiedPush app is installed.
                "android.permission.FOREGROUND_SERVICE_SPECIAL_USE",
            ),
            requested,
        )
    }

    @Test
    fun noFirebaseOrGooglePlayServices() {
        // Pushes come through UnifiedPush or the app's own connection (push/PushRegistrar.kt).
        val build = File("build.gradle.kts").readText() + File("../build.gradle.kts").readText() + File("../gradle/libs.versions.toml").readText()
        val google = Regex("""firebase|google-services|com\.google\.(android\.)?gms|play-services""", RegexOption.IGNORE_CASE)
        assertEquals(emptyList<String>(), google.findAll(build).map { it.value }.toList())
        // The manifest mentions gms only to strip the advertising-ID permission a library might add.
        val manifestMentions = manifest.lines().filter { google.containsMatchIn(it) }
        assertEquals(listOf("""    <uses-permission android:name="com.google.android.gms.permission.AD_ID" tools:node="remove" />"""), manifestMentions)
        val sourceMentions = sources.filter { (_, code) -> Regex("""com\.google\.firebase|com\.google\.android\.gms""").containsMatchIn(code) }.map { it.first.name }
        assertEquals(emptyList<String>(), sourceMentions)
    }

    @Test
    fun everyHttpClientIsTheTorRoutedOneExceptCallSignalling() {
        // docs/tor-by-default.md rule 3: every OkHttp client is built in ApiClient, and the app's two (signed in, and
        // public reads) get the Tor gate. Only LiveKit's group-call signalling has its own: calls never go through Tor.
        val builders = sources.filter { (_, code) -> "OkHttpClient.Builder()" in code || Regex("""OkHttpClient\(\)""").containsMatchIn(code) }.map { it.first.name }.toSet()
        assertEquals(setOf("ApiClient.kt"), builders)
        val app = sources.single { it.first.name == "RedvaultApp.kt" }.second
        assertTrue("ApiClient.httpClient(cookieJar, BuildConfig.API_BASE, tor)" in app)
        assertTrue("ApiClient.publicClient(BuildConfig.API_BASE, tor)" in app)
        assertEquals(listOf("signalingClient = { host -> ApiClient.signalingClient(host) },"),
            app.lines().filter { "ApiClient.signalingClient(" in it }.map { it.trim() })
        // The device-key API is built on the same Tor-routed client.
        assertTrue("RetrofitV3Api(ApiClient.createV3(BuildConfig.API_BASE, httpClient))" in app)
        assertTrue(Regex("""createV3\((?!BuildConfig\.API_BASE, httpClient\))""").find(app) == null)
        // No push service run by anyone else through Tor (docs/tor-by-default.md rule 6).
        val push = sources.single { it.first.name == "PushRegistrar.kt" }.second
        assertTrue("get() = torBuild || prefs.getBoolean(\"strictNoPush\", false)" in push)
        assertTrue("!strictNoPush && !torRouted && isValidVapidKey(vapidKey) && hasDistributor -> PushRoute.UnifiedPush" in push)
    }

    @Test
    fun nothingIsBackedUpToGoogle() {
        assertTrue(manifest.contains("""android:allowBackup="false""""))
    }

    @Test
    fun noAnalyticsCrashReportingOrLocationLibraries() {
        val build = File("build.gradle.kts").readText() + File("../gradle/libs.versions.toml").readText()
        val trackers = Regex(
            "analytics|crashlytics|firebase-perf|play-services-location|play-services-ads|measurement|sentry|bugsnag|mixpanel|amplitude|appsflyer|adjust|segment|flurry|onesignal",
            RegexOption.IGNORE_CASE,
        )
        assertEquals(emptyList<String>(), trackers.findAll(build).map { it.value }.toList())
    }

    @Test
    fun noSourceFileReadsLocationOrFingerprintsTheDevice() {
        val forbidden = listOf(
            "location" to Regex("""LocationManager|FusedLocation|LocationServices|getLastKnownLocation|requestLocationUpdates|Geocoder"""),
            "device identifiers" to Regex("""ANDROID_ID|Settings\.Secure|Build\.(SERIAL|MODEL|MANUFACTURER|DEVICE|FINGERPRINT|getSerial)|TelephonyManager|getImei|getMacAddress|WifiInfo|AdvertisingIdClient"""),
            "installed-app or account snooping" to Regex("""getInstalledPackages|getInstalledApplications|AccountManager|ContactsContract"""),
            "logging in release builds" to Regex("""Log\.[vdiwe]\("""),
            "other network stacks" to Regex("""HttpURLConnection|openConnection\(|java\.net\.URL\(|WebView"""),
        )
        for ((what, pattern) in forbidden) {
            val offenders = sources.filter { (_, code) -> code.lines().any { line -> pattern.containsMatchIn(line) && !line.contains("if (BuildConfig.DEBUG)") } }
                .map { it.first.name }
            assertEquals("found $what in", emptyList<String>(), offenders)
        }
    }
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.update

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.provider.Settings
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.File
import java.io.IOException
import java.security.MessageDigest

/**
 * Checks for, downloads and installs app updates (the website APK; docs: update/UpdateManifest.kt).
 *
 * - The manifest and the APK come from the API server's copy of the published folder, through [client]: the same
 *   Tor-routed client as everything else, so an update check never leaves Tor and carries no identifiers.
 * - Checked once when the app starts (once Tor is up) and then every 24 hours, or from Settings › About.
 * - The APK is saved in the app's cache, checked (size, SHA-256), then handed to Android's own installer
 *   (PackageInstaller), which asks the user to confirm. Android also refuses it unless it's signed with this app's key.
 */
class AppUpdates(
    private val context: Context,
    private val client: OkHttpClient,
    /** "http://<onion>" (or a debug build's test server): where the published folder's copy is. */
    private val apiOrigin: String,
    private val currentVersionCode: Long,
    private val verifyEd25519: (ByteArray, ByteArray, ByteArray) -> Boolean,
) {
    sealed interface State {
        data object Idle : State
        data object Checking : State
        data class UpToDate(val stale: Boolean) : State
        data class Available(val release: UpdateManifest.Release, val stale: Boolean) : State
        data class Downloading(val release: UpdateManifest.Release, val percent: Int) : State
        /** Downloaded and checked; installing needs the user to allow installs from this app first. */
        data class NeedsPermission(val release: UpdateManifest.Release) : State
        data class Installing(val release: UpdateManifest.Release) : State
        data class Failed(val message: String, val release: UpdateManifest.Release? = null) : State
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val _state = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = _state.asStateFlow()
    /** "Later" on the banner: hidden until the next start or a newer version. */
    private val _dismissedVersionCode = MutableStateFlow(0L)
    val dismissedVersionCode: StateFlow<Long> = _dismissedVersionCode.asStateFlow()
    private var periodic: Job? = null
    // The release key; a debug build may be given a test key instead (-PredvaultUpdateTestKey), a release build never.
    private val publicKey = java.util.Base64.getDecoder().decode(
        com.redvaultsystems.redvaultmessage.BuildConfig.UPDATE_TEST_KEY.takeIf { com.redvaultsystems.redvaultmessage.BuildConfig.DEBUG && it.isNotEmpty() }
            ?: UpdateManifest.PUBLIC_KEY_B64,
    )

    init {
        instance = this
    }

    /** Starts the daily check (call once Tor is connected). Safe to call again. */
    fun startPeriodicChecks() {
        if (periodic?.isActive == true) return
        periodic = scope.launch {
            while (true) {
                check()
                delay(CHECK_EVERY_MILLIS)
            }
        }
    }

    fun dismiss(release: UpdateManifest.Release) {
        _dismissedVersionCode.value = release.versionCode
    }

    /** One check (also "Check for updates" in Settings › About). */
    fun checkNow() {
        scope.launch { check() }
    }

    private fun check() {
        val current = _state.value
        if (current is State.Downloading || current is State.Installing || current is State.Checking) return
        _state.value = State.Checking
        _state.value = try {
            val request = Request.Builder().url(UpdateManifest.manifestUrl(apiOrigin)).get().build()
            client.newCall(request).execute().use { response ->
                val type = response.header("Content-Type").orEmpty()
                // No Android release published yet: nginx answers a missing file with a 404 (or the site's page).
                if (response.code == 404 || (response.code == 200 && type.startsWith("text/html"))) return@use State.UpToDate(stale = false)
                if (response.code != 200) return@use State.Failed("The update server answered ${response.code}")
                val body = response.body?.string() ?: return@use State.Failed("The update server sent nothing")
                val result = UpdateManifest.check(body, publicKey, currentVersionCode, System.currentTimeMillis(), verifyEd25519)
                result.release?.let { State.Available(it, result.stale) } ?: State.UpToDate(result.stale)
            }
        } catch (e: UpdateManifest.Invalid) {
            State.Failed(e.message ?: "The update couldn't be checked")
        } catch (e: IOException) {
            State.Failed("Couldn't check for updates: the server can't be reached right now")
        }
    }

    /** "Update": download, check, then install (asking for the install permission first if needed). */
    fun update(release: UpdateManifest.Release) {
        if (_state.value is State.Downloading || _state.value is State.Installing) return
        scope.launch {
            val apk = try {
                download(release)
            } catch (e: IOException) {
                _state.value = State.Failed(e.message ?: "The download failed", release)
                return@launch
            }
            if (!canInstall()) {
                _state.value = State.NeedsPermission(release)
                return@launch
            }
            install(release, apk)
        }
    }

    /** After the user allowed installs from this app: install the APK already downloaded and checked. */
    fun continueInstall(release: UpdateManifest.Release) {
        scope.launch {
            val apk = apkFile(release)
            if (!apk.exists() || !matches(apk, release)) return@launch update(release)
            if (!canInstall()) {
                _state.value = State.NeedsPermission(release)
                return@launch
            }
            install(release, apk)
        }
    }

    fun canInstall(): Boolean = Build.VERSION.SDK_INT < Build.VERSION_CODES.O || context.packageManager.canRequestPackageInstalls()

    /** Android's settings page where the user allows this app to install updates. */
    fun installPermissionIntent(): Intent =
        Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${context.packageName}"))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

    private fun apkFile(release: UpdateManifest.Release) = File(File(context.cacheDir, "updates"), "RedVaultMessages-${release.versionCode}.apk")

    private fun download(release: UpdateManifest.Release): File {
        val dir = File(context.cacheDir, "updates").apply { mkdirs() }
        // Only this version's file is kept.
        dir.listFiles()?.forEach { it.delete() }
        val target = apkFile(release)
        val partial = File(dir, target.name + ".part")
        _state.value = State.Downloading(release, 0)
        val request = Request.Builder().url(UpdateManifest.apkUrl(release, apiOrigin)).get().build()
        client.newCall(request).execute().use { response ->
            if (response.code != 200) throw IOException("The download failed (${response.code})")
            val body = response.body ?: throw IOException("The download was empty")
            val digest = MessageDigest.getInstance("SHA-256")
            var done = 0L
            body.byteStream().use { input ->
                partial.outputStream().use { output ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        val read = input.read(buffer)
                        if (read < 0) break
                        done += read
                        if (done > release.size) throw IOException("The download is larger than the update says")
                        digest.update(buffer, 0, read)
                        output.write(buffer, 0, read)
                        _state.value = State.Downloading(release, ((done * 100) / release.size).toInt())
                    }
                }
            }
            val hash = digest.digest().joinToString("") { "%02x".format(it) }
            if (done != release.size || hash != release.sha256) {
                partial.delete()
                throw IOException("The download doesn't match the signed update; nothing was installed")
            }
        }
        if (!partial.renameTo(target)) throw IOException("Couldn't save the update")
        return target
    }

    private fun matches(file: File, release: UpdateManifest.Release): Boolean {
        if (file.length() != release.size) return false
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val read = input.read(buffer)
                if (read < 0) break
                digest.update(buffer, 0, read)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) } == release.sha256
    }

    private fun install(release: UpdateManifest.Release, apk: File) {
        _state.value = State.Installing(release)
        try {
            val installer = context.packageManager.packageInstaller
            val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
                setAppPackageName(context.packageName)
                setSize(apk.length())
            }
            val sessionId = installer.createSession(params)
            installer.openSession(sessionId).use { session ->
                apk.inputStream().use { input ->
                    session.openWrite("RedVaultMessages.apk", 0, apk.length()).use { output ->
                        input.copyTo(output)
                        session.fsync(output)
                    }
                }
                val intent = Intent(context, UpdateInstallReceiver::class.java).setPackage(context.packageName)
                val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0)
                session.commit(PendingIntent.getBroadcast(context, sessionId, intent, flags).intentSender)
            }
        } catch (e: Exception) {
            _state.value = State.Failed("Android couldn't start the installation: ${e.message}", release)
        }
    }

    /** From [UpdateInstallReceiver]: the installer finished without installing (cancelled or refused). */
    internal fun installEnded(message: String?) {
        val release = (_state.value as? State.Installing)?.release
        _state.value = State.Failed(message ?: "The update wasn't installed", release)
    }

    companion object {
        const val CHECK_EVERY_MILLIS = 24L * 60 * 60 * 1000
        @Volatile internal var instance: AppUpdates? = null
    }
}

/** Android's installer reports back here: show its confirmation screen, or say what went wrong. */
class UpdateInstallReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                @Suppress("DEPRECATION")
                val confirm = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                } else {
                    intent.getParcelableExtra(Intent.EXTRA_INTENT)
                }
                confirm?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)?.let(context::startActivity)
            }
            PackageInstaller.STATUS_SUCCESS -> Unit // The app is replaced and restarts as the new version.
            else -> AppUpdates.instance?.installEnded(intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE))
        }
    }
}

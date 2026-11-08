// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.tor

import android.net.Uri
import android.util.Log
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.goterl.lazysodium.LazySodiumAndroid
import com.goterl.lazysodium.SodiumAndroid
import com.redvaultsystems.redvaultmessage.BuildConfig
import com.redvaultsystems.redvaultmessage.RedvaultApp
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.data.AuthRepository
import com.redvaultsystems.redvaultmessage.data.ConversationRepository
import com.redvaultsystems.redvaultmessage.data.FileRepository
import com.redvaultsystems.redvaultmessage.data.FriendRepository
import com.redvaultsystems.redvaultmessage.data.HiddenMessagesStore
import com.redvaultsystems.redvaultmessage.data.KeyPinStore
import com.redvaultsystems.redvaultmessage.data.KeyTrust
import com.redvaultsystems.redvaultmessage.data.MessageKeys
import com.redvaultsystems.redvaultmessage.data.SignupConsent
import com.redvaultsystems.redvaultmessage.data.SigningKeyPinStore
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.SessionCookieJar
import com.redvaultsystems.redvaultmessage.data.model.Message
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.identity.LastHandleStore
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import com.redvaultsystems.redvaultmessage.crypto.RecoveryKey
import com.redvaultsystems.redvaultmessage.push.PushRegistrar
import com.redvaultsystems.redvaultmessage.ui.files.LockChoice
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.net.Socket
import java.security.MessageDigest
import kotlin.random.Random

/**
 * The real Android app over Tor (docs/tor-routing-plan.md), against the
 * scratch API behind a test onion service (tor-core/poc/harness). Runs only
 * when asked: am instrument -e torProbe true ... (and an APK built with
 * -PredvaultApiBase=http://<onion>/api/). Each step is logged as one JSON
 * line under the "TorProbe" tag; the test fails if any step did.
 *
 * The harness's control port (the host's 127.0.0.1:18082, 10.0.2.2 from the
 * emulator) is read directly by this test, never by the app: it's how the
 * test sees which listener every request arrived on.
 */
@RunWith(AndroidJUnit4::class)
class TorAppProbeTest {
    private var failures = 0

    private fun step(name: String, ok: Boolean, vararg detail: Pair<String, Any?>) {
        val line = buildJsonObject {
            put("step", name)
            put("ok", ok)
            put("detail", buildJsonObject { detail.forEach { (k, v) -> put(k, v?.toString()) } })
        }
        Log.i("TorProbe", line.toString())
        if (!ok) failures++
    }

    private fun waitFor(ms: Long, condition: () -> Boolean): Boolean {
        val end = System.currentTimeMillis() + ms
        while (System.currentTimeMillis() < end) {
            if (runCatching(condition).getOrDefault(false)) return true
            Thread.sleep(250)
        }
        return false
    }

    /** The harness's control port, over a plain socket (the app's network security config doesn't apply to it). */
    private fun control(path: String, body: String? = null): String {
        Socket("10.0.2.2", 18082).use { socket ->
            val request = if (body == null) {
                "GET $path HTTP/1.0\r\nHost: control\r\n\r\n"
            } else {
                "POST $path HTTP/1.0\r\nHost: control\r\nContent-Type: application/json\r\nContent-Length: ${body.toByteArray().size}\r\n\r\n$body"
            }
            socket.getOutputStream().write(request.toByteArray())
            return socket.getInputStream().readBytes().toString(Charsets.UTF_8).substringAfter("\r\n\r\n")
        }
    }

    private fun serverLog(): List<JsonObject> =
        Json.parseToJsonElement(control("/log")).jsonObject["log"]!!.jsonArray.map { it.jsonObject }

    private fun partPuts(log: List<JsonObject>, part: Int = 0): Int = log.count {
        val path = it["path"]!!.jsonPrimitive.content
        it["method"]!!.jsonPrimitive.content == "PUT" && path.contains("/parts/") && it["status"]!!.jsonPrimitive.int == 200 &&
            (part == 0 || path.endsWith("/parts/$part"))
    }

    private fun direct(log: List<JsonObject>): Int = log.count { it["listener"]!!.jsonPrimitive.content != "onion" }

    private fun sha256(file: File): String = MessageDigest.getInstance("SHA-256").digest(file.readBytes()).joinToString("") { "%02x".format(it) }

    @Test
    fun theRealAppOverTor() = runBlocking {
        assumeTrue("run with -e torProbe true", InstrumentationRegistry.getArguments().getString("torProbe") == "true")
        val app = ApplicationProvider.getApplicationContext<RedvaultApp>()
        val c = app.container
        val tor = c.tor
        step("tor_enabled_for_the_onion_api_base", tor != null, "apiBase" to BuildConfig.API_BASE)
        if (tor == null) return@runBlocking
        // A UnifiedPush distributor would reach its push server outside Tor (by design, and documented): off for this run.
        c.pushRegistrar.strictNoPush = true
        step("tor_connects", waitFor(300_000) { tor.connected }, "detail" to tor.status.value.detail)
        control("/log/clear", "{}")

        // One plain request through the app's own Tor client first, with the full error if it fails.
        val probeClient = ApiClient.httpClient(SessionCookieJar(), BuildConfig.API_BASE, tor)
        // A first circuit to an onion service sometimes fails (a rendezvous that doesn't come up): try again, as the apps do.
        var attempts = 0
        var first: Result<Pair<Int, String?>>
        do {
            attempts++
            first = runCatching {
                probeClient.newCall(okhttp3.Request.Builder().url(BuildConfig.API_BASE + "public/routing").build()).execute().use { it.code to it.body?.string() }
            }
            if (first.isFailure) Thread.sleep(10_000)
        } while (first.isFailure && attempts < 6)
        step(
            "first_request_through_tor",
            first.getOrNull()?.first == 200,
            "attempts" to attempts,
            "result" to first.getOrNull(),
            "error" to first.exceptionOrNull()?.let { e -> generateSequence(e) { it.cause }.joinToString(" <- ") { "${it.javaClass.name}: ${it.message}" } },
        )

        // Bob: a second account in this process, with its own cookies, through the same Tor gate.
        val sodium = LazySodiumAndroid(SodiumAndroid())
        val crypto = E2eCrypto(sodium)
        val bobHttp = ApiClient.httpClient(SessionCookieJar(), BuildConfig.API_BASE, tor)
        val bobApi = ApiClient.create(BuildConfig.API_BASE, bobHttp)
        val bobAuth = AuthRepository(bobApi, crypto, SessionCookieJar(), LastHandleStore(app), PushRegistrar(app, ApiClient.createPush(BuildConfig.API_BASE, bobHttp)), RecoveryKey(sodium, crypto))
        val suffix = Random.nextInt(100000, 999999)
        val password = "correct horse battery staple $suffix"
        val consent = SignupConsent(termsAccepted = true, privacyAcknowledged = true, age18Confirmed = true)
        val alice: SelfIdentity
        val bob: SelfIdentity
        suspend fun <T> retried(block: suspend () -> T): T {
            var last: Exception? = null
            repeat(4) {
                try {
                    return block()
                } catch (e: Exception) {
                    last = e
                    Thread.sleep(10_000)
                }
            }
            throw last!!
        }
        try {
            alice = retried { c.authRepository.register("andalice$suffix", password, consent).identity }
            bob = retried { bobAuth.register("andbob$suffix", password, consent).identity }
            step("register_over_tor", true)
        } catch (e: Exception) {
            step("register_over_tor", false, "error" to e.toString())
            throw AssertionError("registration failed")
        }
        control("/tier", "{\"userId\":\"${alice.userId}\",\"tier\":\"pro\"}")
        control("/tier", "{\"userId\":\"${bob.userId}\",\"tier\":\"pro\"}")

        var session = c.createSession(alice) {}
        retried { session.account.sendFriendRequest("andbob$suffix") }
        val bobFriends = FriendRepository(bobApi, crypto)
        val request = retried { bobFriends.pending().incoming.first() }
        val chatId = retried { bobFriends.accept(bob, request) }
        session.chats.refreshNow()
        step("friend_and_chat", waitFor(120_000) { session.chats.conversation(chatId) != null })

        val bobKeys = MessageKeys(bobApi, crypto, SigningKeyPinStore(app), KeyTrust(crypto, KeyPinStore(app))).also { it.begin(bob) }
        val bobRepo = ConversationRepository(bobApi, crypto, HiddenMessagesStore(app), bobKeys)
        suspend fun bobSees(): List<Message> {
            val dto = bobRepo.listConversations().first { it.id == chatId }
            val meta = bobRepo.hydrateMeta(dto, bob)!!
            return bobRepo.fetchMessages(meta, bob.userId).messages
        }
        fun countOf(messages: List<Message>, text: String) = messages.count { it.text == text }

        // A: text and a photo, through the outbox and Tor.
        val textA = "Hello from Android over Tor $suffix"
        session.chats.send(chatId, textA) {}
        val drainedA = waitFor(180_000) { session.chats.state.value.queued == 0 && runBlocking { countOf(bobSees(), textA) } == 1 }
        step("text_via_outbox_and_tor", drainedA)

        val dir = File(app.cacheDir, "probe").apply { mkdirs() }
        val photo = File(dir, "android-photo.jpg").apply { writeBytes(Random(4).nextBytes(250_000)) }
        var photoError: String? = "pending"
        session.chats.sendFile(chatId, Uri.fromFile(photo), {}) { photoError = it }
        val photoSent = waitFor(240_000) { photoError == null && session.chats.state.value.queued == 0 }
        val bobFiles = FileRepository(bobApi, bobRepo, FileCrypto(sodium), bobHttp, BuildConfig.API_BASE, app.contentResolver, File(app.cacheDir, "bob"))
        // Bob's side is the test's own client: a dropped circuit there is retried, not counted against the app.
        val received = retried { bobSees() }.firstOrNull { it.kind == MessageKind.File && it.text == "android-photo.jpg" }
        val photoOk = runCatching { received?.file?.let { file -> sha256(retried { bobFiles.download(file) {} }) == sha256(photo) } == true }.getOrDefault(false)
        step("photo_via_outbox_and_tor", photoSent && photoOk, "error" to photoError, "bobSawIt" to (received != null))
        val logA = serverLog()
        step("phase_a_only_onion", direct(logA) == 0 && logA.isNotEmpty(), "onion" to logA.size - direct(logA), "direct" to direct(logA))

        // Files tab: a resumable upload in parts, cut off by Tor stopping.
        val backup = File(dir, "android-backup.bin").apply { writeBytes(Random(5).nextBytes(20 * 1024 * 1024)) }
        val backupHash = sha256(backup)
        session.files.refreshNow()
        val queuedUpload = session.files.upload(listOf(Uri.fromFile(backup)), listOf("android-backup.bin" to backup.length()), "", LockChoice("none"))
        val firstPart = waitFor(300_000) { partPuts(serverLog()) >= 1 }
        step("files_upload_goes_in_parts", queuedUpload == null && firstPart, "error" to queuedUpload)

        // B: Tor stops. Nothing may go out; everything waits, sealed.
        tor.stop()
        step("tor_stopped_reports_unavailable", tor.status.value.state == TorState.Unavailable)
        val paused = waitFor(120_000) { session.files.state.value.uploads.any { it.stage == "paused" } }
        val partsAtStop = partPuts(serverLog())
        step("files_upload_pauses_when_tor_stops", paused && partsAtStop in 1..2, "parts" to partsAtStop)
        Thread.sleep(2_000)
        val before = serverLog().size
        val textB = "Written while Tor was down $suffix"
        session.chats.send(chatId, textB) {}
        val doc = File(dir, "android-report.pdf").apply { writeBytes(Random(6).nextBytes(180_000)) }
        var docError: String? = "pending"
        session.chats.sendFile(chatId, Uri.fromFile(doc), {}) { docError = it }
        waitFor(30_000) { docError == null }
        Thread.sleep(5_000) // gives any (wrong) send the chance to show up
        val after = serverLog().size
        val shownQueued = session.chats.conversation(chatId)!!.messages.any { it.queued && it.text == textB }
        step(
            "tor_down_queues_and_sends_nothing",
            session.chats.state.value.queued == 2 && after == before && shownQueued,
            "queued" to session.chats.state.value.queued, "serverRequestsWhileDown" to (after - before), "shownAsQueued" to shownQueued,
        )
        val outboxDisk = File(app.noBackupFilesDir, "outbox").walkTopDown().filter { it.isFile }.map { it.readBytes() }.fold(ByteArray(0)) { a, b -> a + b }
        val diskText = String(outboxDisk, Charsets.ISO_8859_1)
        step(
            "outbox_sealed_on_disk",
            outboxDisk.isNotEmpty() && !diskText.contains(textB) && !diskText.contains(String(doc.readBytes().copyOfRange(1000, 1064), Charsets.ISO_8859_1)),
            "bytes" to outboxDisk.size,
        )

        // C: a restart (a new session over the same sealed outbox and journal), then Tor comes back.
        session.close()
        tor.start()
        step("restart_tor_connects", waitFor(300_000) { tor.connected }, "detail" to tor.status.value.detail)
        session = c.createSession(alice) {}
        val restored = session.files.state.value.uploads.firstOrNull { it.name == "android-backup.bin" }
        step("files_upload_restored_from_journal", restored != null, "stage" to restored?.stage)
        session.chats.refreshNow()
        val drainedC = waitFor(300_000) {
            runBlocking { session.chats.refreshNow() }
            val seen = runBlocking { bobSees() }
            session.chats.state.value.queued == 0 && countOf(seen, textB) == 1 && seen.count { it.text == "android-report.pdf" } == 1
        }
        val seenC = retried { bobSees() }
        step("queue_drained_once_after_restart", drainedC, "textCopies" to countOf(seenC, textB), "fileCopies" to seenC.count { it.text == "android-report.pdf" })

        val uploaded = waitFor(600_000) {
            runBlocking { session.files.refreshNow() }
            session.files.state.value.uploads.isEmpty() && session.files.state.value.currentFiles.any { it.name == "android-backup.bin" }
        }
        val logC = serverLog()
        step("files_upload_resumed_without_resending", uploaded && partPuts(logC) == 3 && partPuts(logC, 1) == 1, "parts" to partPuts(logC), "part1" to partPuts(logC, 1))
        var downloadedOk = false
        if (uploaded) {
            val entry = session.files.state.value.currentFiles.first { it.name == "android-backup.bin" }
            val out = File(dir, "android-backup-restored.bin")
            val error = session.files.saveTo(entry.id, out.outputStream())
            downloadedOk = error == null && sha256(out) == backupHash
        }
        step("files_download_decrypts_to_original", downloadedOk)
        val logEnd = serverLog()
        step("all_server_traffic_via_onion", direct(logEnd) == 0, "onion" to logEnd.size - direct(logEnd), "direct" to direct(logEnd))
        session.close()
        Log.i("TorProbe", "{\"summary\":\"${if (failures == 0) "all steps passed" else "$failures step(s) failed"}\"}")
        assertEquals("failed steps (see logcat TorProbe)", 0, failures)
    }
}

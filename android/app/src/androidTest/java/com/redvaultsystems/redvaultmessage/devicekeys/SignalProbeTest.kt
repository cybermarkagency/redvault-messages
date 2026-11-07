package com.redvaultsystems.redvaultmessage.devicekeys

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.LoginRequest
import com.redvaultsystems.redvaultmessage.data.api.SessionCookieJar
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.io.PrintWriter
import java.net.Socket
import java.util.Base64

/**
 * The Android side of the M4 cross-platform run (docs/signal-integration/, development only). A
 * driver, not a check: it does nothing unless the runner passes `-e signalProbePort <port>`.
 *
 * On the real Android runtime it uses the app's own device-key code (V3Device, maintainSignal,
 * sendText, receiveMessages), its HTTP client (RetrofitV3Api), the Android Keystore key wrapper
 * and the engine's JNI library built with `signal`. It takes one JSON command per line from the
 * runner on the computer, through `adb reverse`, and answers one JSON line. It signs in to a
 * throwaway dev server with a dev account. No private value is ever printed.
 */
@RunWith(AndroidJUnit4::class)
class SignalProbeTest {
    private class Probe {
        lateinit var baseUrl: String
        lateinit var userId: String
        lateinit var legacyKey: ByteArray
        lateinit var api: V3Api
        lateinit var storage: DeviceStorage
        lateinit var engine: EngineRunner
        lateinit var dir: File
        var device: V3Device? = null
        var history: ChatHistory? = null
    }

    @Test fun drive() {
        val port = InstrumentationRegistry.getArguments().getString("signalProbePort")?.toIntOrNull()
        assumeTrue("only for the M4 runner (-e signalProbePort)", port != null)
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val call = NativeEngine.call()
        assumeTrue("the engine library isn't in this build", call != null)
        val p = Probe().apply { engine = EngineRunner(call!!) }
        Socket("127.0.0.1", port!!).use { socket ->
            val input = BufferedReader(InputStreamReader(socket.getInputStream(), Charsets.UTF_8))
            val output = PrintWriter(socket.getOutputStream().bufferedWriter(Charsets.UTF_8), true)
            output.println(buildJsonObject { put("ready", true); put("sdk", android.os.Build.VERSION.SDK_INT) })
            while (true) {
                val line = input.readLine() ?: break
                val cmd = Json.parseToJsonElement(line).jsonObject
                val name = cmd["cmd"]!!.jsonPrimitive.content
                if (name == "quit") {
                    output.println(buildJsonObject { put("ok", true) })
                    break
                }
                val answer = try {
                    val result = runBlocking { handle(p, name, cmd, context.noBackupFilesDir) }
                    buildJsonObject { put("ok", true); put("result", result) }
                } catch (e: Throwable) {
                    val code = (e as? EngineException)?.code ?: (e as? com.redvaultsystems.redvaultmessage.data.api.ApiException)?.code ?: e.javaClass.simpleName
                    buildJsonObject { put("ok", false); put("error", code); put("message", (e.message ?: "").take(300)) }
                }
                output.println(answer)
            }
        }
    }

    private fun str(cmd: JsonObject, field: String) = cmd[field]!!.jsonPrimitive.content

    private suspend fun handle(p: Probe, name: String, cmd: JsonObject, base: File): JsonObject = when (name) {
        "init" -> {
            p.baseUrl = str(cmd, "baseUrl")
            val jar = SessionCookieJar()
            val client = ApiClient.httpClient(jar, p.baseUrl)
            val login = ApiClient.create(p.baseUrl, client).login(LoginRequest(str(cmd, "username"), str(cmd, "password")))
            p.userId = login.userId!!
            p.legacyKey = Base64.getDecoder().decode(str(cmd, "legacyKey"))
            p.api = RetrofitV3Api(ApiClient.createV3(p.baseUrl, client))
            // A folder per account, under noBackupFilesDir as the app keeps its own.
            p.dir = File(base, "signal-probe/${p.userId}").also { it.mkdirs() }
            p.storage = DeviceStorage(File(p.dir, "e2eV3"), AndroidKeystoreWrapper())
            p.device = V3Device.open(p.userId, p.storage, p.engine, p.api)
            p.history = null
            buildJsonObject {
                put("userId", p.userId)
                put("hasDevice", p.device != null)
                put("protection", p.device?.protection() ?: "none")
            }
        }
        "setup" -> {
            val device = setUpFirstDevice(p.userId, p.legacyKey.copyOf(), p.storage, p.engine, p.api)
            p.device = device
            buildJsonObject { put("deviceId", device.deviceId); put("protection", device.protection()) }
        }
        "reopen" -> {
            // What the app does after a restart: everything again from storage.
            p.device = V3Device.open(p.userId, p.storage, p.engine, p.api)
            p.history = null
            buildJsonObject { put("hasDevice", p.device != null); put("deviceId", p.device?.deviceId ?: "") }
        }
        "signal" -> buildJsonObject {
            put("outcome", maintainSignal(p.device!!))
            put("engine", engineHasSignal(p.device!!))
        }
        "chatSend" -> {
            // `group: true` sends to a group (M6: Signal sender keys when every member device speaks Signal).
            val group = cmd["group"]?.jsonPrimitive?.contentOrNull == "true"
            val outcome = if (group) {
                sendGroupText(p.device!!, history(p), str(cmd, "conversationId"), str(cmd, "text"))
            } else {
                sendText(p.device!!, history(p), str(cmd, "conversationId"), str(cmd, "contactUserId"), str(cmd, "text"))
            }
            buildJsonObject {
                put("sent", outcome is SendOutcome.Sent)
                if (outcome is SendOutcome.Sent) put("id", outcome.record.id)
            }
        }
        "chatReceive" -> {
            val report = receiveMessages(p.device!!, history(p))
            buildJsonObject {
                put("items", buildJsonArray {
                    for (r in report.received) add(buildJsonObject {
                        put("id", r.id); put("from", r.fromUserId); put("text", r.text); put("direction", r.direction); put("conversationId", r.conversationId)
                    })
                })
                put("held", JsonPrimitive(report.held.size))
            }
        }
        "safety" -> {
            // What the Verify screen shows: the number, the code for the other camera, and whether it was verified.
            val safety = deviceKeySafety(p.device!!, str(cmd, "contactUserId"))
            buildJsonObject {
                if (safety == null) {
                    put("groups", JsonNull)
                } else {
                    put("groups", buildJsonArray { safety.groups.forEach { add(JsonPrimitive(it)) } })
                    put("qrText", safety.qrText)
                    put("accountKey", safety.accountKey)
                    put("verified", safety.verified)
                    put("keyChanged", safety.keyChanged)
                }
            }
        }
        "safetyScan" -> buildJsonObject {
            put("verified", verifyContactQr(p.device!!, str(cmd, "contactUserId"), str(cmd, "accountKey"), str(cmd, "qrText")))
        }
        "safetyMark" -> {
            setContactVerified(p.device!!, str(cmd, "contactUserId"), str(cmd, "accountKey"), cmd["verified"]!!.jsonPrimitive.content == "true")
            buildJsonObject {}
        }
        else -> throw IllegalArgumentException("unknown command $name")
    }

    private fun history(p: Probe): ChatHistory =
        p.history ?: EncryptedChatHistory(File(p.dir, "chat.hist"), p.device!!.historyKey()).also { p.history = it }

    @Suppress("unused")
    private fun JsonObject.text(field: String) = this[field]?.jsonPrimitive?.contentOrNull
}

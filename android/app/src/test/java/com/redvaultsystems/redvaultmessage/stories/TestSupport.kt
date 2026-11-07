package com.redvaultsystems.redvaultmessage.stories

import com.goterl.lazysodium.LazySodium
import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

/** libsodium on the JVM (the desktop build bundled in lazysodium-java): the same API the app calls through lazysodium-android. */
internal object TestSodium {
    val sodium: LazySodium by lazy { LazySodiumJava(SodiumJava()) }
}

/** A vector file made by the web app's own code (src/test/resources/vectors/). */
internal fun vectors(name: String): JsonObject {
    val stream = checkNotNull(TestSodium::class.java.getResourceAsStream("/vectors/$name")) { "missing vector file $name" }
    val text = stream.use { String(it.readBytes(), Charsets.UTF_8) }
    return Json.parseToJsonElement(text).jsonObject
}

internal fun JsonElement.str(): String = jsonPrimitive.content
internal fun JsonObject.str(key: String): String = getValue(key).str()
internal fun JsonObject.obj(key: String): JsonObject = getValue(key).jsonObject
internal fun JsonObject.arr(key: String): JsonArray = getValue(key).jsonArray
internal fun JsonObject.b64(key: String): ByteArray = Base64Codec.decode(str(key))

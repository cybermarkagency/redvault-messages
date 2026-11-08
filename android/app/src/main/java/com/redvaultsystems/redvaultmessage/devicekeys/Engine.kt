// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.e2ee.NativeE2ee
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

/**
 * The shared encryption engine (crypto-core: vodozemac Olm and Megolm, device lists, pairing) as one
 * JSON request in, one JSON response out (docs/multi-device-threat-model.md). Builds include it unless made with
 * -PredvaultE2eV3=false. Nothing here talks to the network.
 */
fun interface EngineCall {
    fun call(request: String): String
}

/** The engine refused, or isn't there. [code] is the engine's machine-readable error. */
class EngineException(val code: String, message: String) : Exception(message)

/** An engine response. [state] is the device's new encrypted state when the call changed it. */
class EngineResponse(val json: JsonObject) {
    val ok: Boolean get() = json["ok"]?.jsonPrimitive?.booleanOrNull == true
    val error: String? get() = json["error"]?.jsonPrimitive?.contentOrNull
    val message: String? get() = json["message"]?.jsonPrimitive?.contentOrNull
    val state: String? get() = json["state"]?.jsonPrimitive?.contentOrNull

    operator fun get(field: String): JsonElement? = json[field]
    fun string(field: String): String = json[field]?.jsonPrimitive?.contentOrNull ?: throw EngineException("bad_response", "the engine's answer has no $field")
    fun obj(field: String): JsonObject = json[field]?.jsonObject ?: throw EngineException("bad_response", "the engine's answer has no $field")
    fun array(field: String): JsonArray = json[field]?.jsonArray ?: throw EngineException("bad_response", "the engine's answer has no $field")
    fun bool(field: String): Boolean? = json[field]?.jsonPrimitive?.booleanOrNull
}

/** The JNI binding (com.redvaultsystems.e2ee.NativeE2ee), or null when the library isn't in this build. */
object NativeEngine {
    private val loaded: Boolean by lazy { runCatching { NativeE2ee.load() }.isSuccess }

    /** Whether this build includes the engine at all. */
    fun available(): Boolean = loaded

    fun call(): EngineCall? = if (loaded) EngineCall { NativeE2ee.call(it) } else null
}

/** Runs requests against [engine]; used by the device and by the stateless pairing steps. */
class EngineRunner(private val engine: EngineCall) {
    fun run(request: JsonObject): EngineResponse =
        EngineResponse(Json.parseToJsonElement(engine.call(request.toString())).jsonObject)

    fun must(request: JsonObject): EngineResponse {
        val response = run(request)
        if (!response.ok) throw EngineException(response.error ?: "engine_error", response.message ?: "the engine refused")
        return response
    }

    fun must(op: String, vararg fields: Pair<String, Any?>): EngineResponse = must(request(op, *fields))
}

/** A request object: strings, numbers, booleans and JSON values; nulls are left out. */
fun request(op: String, vararg fields: Pair<String, Any?>): JsonObject = buildJsonObject {
    put("op", op)
    for ((name, value) in fields) {
        when (value) {
            null -> {}
            is JsonElement -> put(name, value)
            is String -> put(name, value)
            is Boolean -> put(name, value)
            is Number -> put(name, JsonPrimitive(value))
            else -> error("unsupported field $name")
        }
    }
}

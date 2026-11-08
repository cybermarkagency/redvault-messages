// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.serialization.json.jsonPrimitive

/**
 * Everything a Verify screen shows for the device-key safety number (the same number, code and
 * rules as the desktop app: desktop/e2ev3/src/device.cpp, crypto-core/src/identity.rs).
 */
data class DeviceKeySafety(
    /** 12 groups of 5 digits. */
    val groups: List<String>,
    /** "RVDK1:..." for the other person's camera. */
    val qrText: String,
    /** The contact's account key the number is over. */
    val accountKey: String,
    /** This account key was verified on this phone. */
    val verified: Boolean,
    /** The contact's signed list has a new account key that this phone hasn't trusted yet. */
    val keyChanged: Boolean,
)

/**
 * The device-key safety number with a contact, or null while either has no device keys. After a
 * key change this is the number over the NEW key (from a list that key signed) with keyChanged set,
 * so the person can compare it before accepting the key with [acceptContactKey].
 */
suspend fun deviceKeySafety(device: V3Device, contactUserId: String, conversationId: String? = null): DeviceKeySafety? {
    val list = device.api.deviceList(contactUserId, conversationId).deviceList ?: return null
    val verified = device.op("verifyDeviceList", "deviceList" to list)
    val fields = mutableListOf<Pair<String, Any?>>("userId" to contactUserId)
    if (!verified.ok) {
        val code = verified.error ?: "bad_device_list"
        // A key change: the list verified under its own (new) key, which isn't the pinned one.
        if (code != "account_key_changed") throw EngineException(code, verified.message ?: "the device list doesn't verify")
        fields += "accountKey" to (list["accountKey"]?.jsonPrimitive?.content ?: throw EngineException("bad_device_list", "the list has no account key"))
    } else if (verified.array("devices").isEmpty()) {
        return null
    }
    val fingerprint = device.must("fingerprint", *fields.toTypedArray())
    return DeviceKeySafety(
        groups = fingerprint.array("safetyNumber").map { it.jsonPrimitive.content },
        qrText = fingerprint.string("qrText"),
        accountKey = fingerprint.string("accountKey"),
        verified = fingerprint.bool("verified") == true,
        keyChanged = fingerprint.bool("keyChanged") == true,
    )
}

/** The person compared the number over [accountKey] and marks it verified, or takes that back. */
fun setContactVerified(device: V3Device, contactUserId: String, accountKey: String, verified: Boolean) {
    device.must("setContactVerified", "userId" to contactUserId, "accountKey" to accountKey, "verified" to verified)
}

/** Accepts a contact's new account key after a key change and re-pins their list; [verified] when the new number was compared. */
suspend fun acceptContactKey(device: V3Device, contactUserId: String, accountKey: String, verified: Boolean, conversationId: String? = null) {
    device.must("trustAccountKey", "userId" to contactUserId, "accountKey" to accountKey, "verified" to verified)
    repin(device, contactUserId, conversationId)
}

/** What the camera read off the other person's screen: true when it matches (and then a changed key is accepted). */
suspend fun verifyContactQr(device: V3Device, contactUserId: String, accountKey: String, qrText: String, conversationId: String? = null): Boolean {
    val result = device.must("verifyFingerprintQr", "userId" to contactUserId, "accountKey" to accountKey, "qrText" to qrText, "acceptNewKey" to true)
    if (result.bool("acceptedNewKey") == true) repin(device, contactUserId, conversationId)
    return result.bool("verified") == true
}

private suspend fun repin(device: V3Device, contactUserId: String, conversationId: String?) {
    val list = device.api.deviceList(contactUserId, conversationId).deviceList ?: return
    val verified = device.op("verifyDeviceList", "deviceList" to list)
    if (!verified.ok) throw EngineException(verified.error ?: "bad_device_list", verified.message ?: "the device list doesn't verify")
}

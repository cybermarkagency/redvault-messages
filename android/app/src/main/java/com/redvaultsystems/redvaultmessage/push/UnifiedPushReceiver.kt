package com.redvaultsystems.redvaultmessage.push

import com.redvaultsystems.redvaultmessage.RedvaultApp
import org.unifiedpush.android.connector.FailedReason
import org.unifiedpush.android.connector.PushService
import org.unifiedpush.android.connector.data.PushEndpoint
import org.unifiedpush.android.connector.data.PushMessage

/**
 * Receives what the user's UnifiedPush distributor (ntfy, for example) sends this app: new endpoints, the
 * server's content-free pushes and unregistrations. The connector has already decrypted each push (Web
 * Push, RFC 8291), so the distributor and its server only ever carried ciphertext; the plaintext says no
 * more than `{"kind":"message"}`.
 */
class UnifiedPushReceiver : PushService() {
    private val registrar get() = (application as RedvaultApp).container.pushRegistrar

    override fun onNewEndpoint(endpoint: PushEndpoint, instance: String) {
        // Without a key set the server couldn't encrypt to us, and it sends only encrypted pushes.
        registrar.onNewEndpoint(endpoint.url, endpoint.pubKeySet?.pubKey, endpoint.pubKeySet?.auth)
    }

    override fun onMessage(message: PushMessage, instance: String) {
        // Not decrypted: not from our server (or damaged on the way). Never shown.
        if (!message.decrypted) return
        if (!registrar.enabled || registrar.strictNoPush) return
        val kind = PushKinds.parsePayload(message.content) ?: return
        PushKinds.notify(this, kind)
    }

    override fun onRegistrationFailed(reason: FailedReason, instance: String) {
        // INTERNAL_ERROR, NETWORK, ACTION_REQUIRED (the user has to sort out the distributor) or
        // VAPID_REQUIRED (the server had none): the background connection covers it until the next sign-in.
        registrar.onPushLost(unregistered = false)
    }

    override fun onUnregistered(instance: String) {
        registrar.onPushLost(unregistered = true)
    }

    override fun onTempUnavailable(instance: String) {
        // The distributor's server is down for now; onNewEndpoint follows when it's back and stops the fallback.
        registrar.onPushLost(unregistered = false)
    }
}

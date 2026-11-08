// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.calls

import android.content.Context
import android.content.Intent
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.CallCrypto
import com.redvaultsystems.redvaultmessage.crypto.CallSignal
import com.redvaultsystems.redvaultmessage.crypto.CallSignalException
import com.redvaultsystems.redvaultmessage.crypto.SealedSignal
import com.redvaultsystems.redvaultmessage.data.CallHistoryStore
import com.redvaultsystems.redvaultmessage.data.CallOutcome
import com.redvaultsystems.redvaultmessage.data.CallRecord
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.CallApi
import com.redvaultsystems.redvaultmessage.data.api.CreateCallLinkRequest
import com.redvaultsystems.redvaultmessage.data.api.IceServerDto
import com.redvaultsystems.redvaultmessage.data.api.ScreenRequest
import com.redvaultsystems.redvaultmessage.data.api.SealedSignalDto
import com.redvaultsystems.redvaultmessage.data.api.StartCallRequest
import com.redvaultsystems.redvaultmessage.data.api.StartedCallDto
import com.redvaultsystems.redvaultmessage.data.api.UserResult
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import com.redvaultsystems.redvaultmessage.ui.live.LiveSession
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.decodeFromJsonElement
import org.webrtc.MediaConstraints
import org.webrtc.MediaStreamTrack
import org.webrtc.PeerConnection
import org.webrtc.RtpTransceiver
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.VideoSink
import org.webrtc.VideoTrack
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

enum class CallPhase { Outgoing, Incoming, Connecting, Active, Reconnecting, Ended }

/** What the call screens show. */
data class CallView(
    /** The 1:1 chat for a call between contacts; null for a call link. */
    val conversationId: String?,
    val linkId: String?,
    val linkName: String?,
    val peerUserId: String,
    val peerHandle: String,
    val peerAvatarUrl: String?,
    /** "voice" or "video". */
    val kind: String,
    /** "outgoing" or "incoming". */
    val direction: String,
    val phase: CallPhase,
    val muted: Boolean = false,
    val cameraOff: Boolean = kind == "voice",
    val speakerOn: Boolean = kind == "video",
    val sharingScreen: Boolean = false,
    /** The other side is sharing their screen (confirmed by the server, which checks their plan). */
    val remoteSharing: Boolean = false,
    /** Frames are arriving from the other side's camera or screen. */
    val remoteVideo: Boolean = false,
    val connectedAt: Long? = null,
    /** Four words from both DTLS fingerprints, once connected. */
    val sas: List<String>? = null,
    /** Why the call ended, or what went wrong, for the closing banner. */
    val endedMessage: String? = null,
    /** A non-fatal problem to show in the call (e.g. screen sharing refused). */
    val notice: String? = null,
)

/** One of your call links, opened with your own keys. */
data class CallLink(val id: String, val kind: String, val name: String, val url: String, val createdOn: String, val secret: ByteArray)

data class CallsState(
    val call: CallView? = null,
    /** False when this server has no TURN relay (or is too old for calls): call buttons are hidden. */
    val available: Boolean = false,
    val history: List<CallRecord> = emptyList(),
    val links: List<CallLink> = emptyList(),
    val linksLoaded: Boolean = false,
    val linkError: String? = null,
    /** The link just created, to copy and share. */
    val createdLinkUrl: String? = null,
)

/**
 * One-to-one voice and video calls with screen sharing, between contacts or
 * through a call link: a port of the web's state/CallContext.tsx.
 *
 *   - Media: WebRTC, encrypted end to end by DTLS-SRTP, forced through the
 *     TURN relay (relay-only ICE), so neither person learns the other's IP.
 *   - Setup: the offer and answer, with each side's DTLS fingerprint and
 *     every relay candidate (no trickle through the server), are sealed end
 *     to end by [CallCrypto]. Once connected both screens show the same four
 *     words, derived from both fingerprints, to say aloud.
 *   - Signaling: the live channel, held open for as long as the call lasts.
 *   - Plans: calls with contacts are free; call links need Pro (voice) or
 *     Premium (video) on the host's side; screen sharing needs Premium.
 *   - History: kept only on this phone, encrypted ([CallHistoryStore]).
 */
class CallManager(
    private val context: Context,
    private val api: CallApi,
    private val lookupUser: suspend (String) -> UserResult,
    private val crypto: CallCrypto,
    private val history: CallHistoryStore,
    private val identity: SelfIdentity,
    private val live: LiveSession,
    private val webAppBase: String,
    /** The 1:1 chats loaded so far (calls ring only from contacts whose key we hold). */
    private val conversations: () -> List<Conversation>,
    private val tier: () -> String,
    /** Settings › Ringtone: ring for incoming calls. */
    private val ringtone: () -> Boolean = { true },
    /** Settings › Echo cancellation. */
    private val echoCancellation: () -> Boolean = { true },
    /** A group call is on (nobody is in two calls at once). */
    private val groupCallBusy: () -> Boolean = { false },
    private val onSessionExpired: () -> Unit,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow(CallsState(history = history.load(identity.userId, identity.privateKey)))
    val state: StateFlow<CallsState> = _state.asStateFlow()

    private val media by lazy { CallMedia(context, echoCancellation) }
    private val audio by lazy { CallAudio(context) }
    private var session: Session? = null
    private var endedJob: Job? = null

    /**
     * An idle peer connection (no relays, sends nothing) that starts WebRTC's
     * network monitor ahead of the call's own and keeps it running. The
     * monitor learns about Wi-Fi or mobile data a few ms after it starts; a
     * call that began gathering before that found "no networks", ended its
     * gathering at once with no relay candidate and failed with "Couldn't
     * reach the call relay". See [warmNetwork] and [awaitNetwork].
     */
    private var networkWarmer: PeerConnection? = null
    private var warmedAt = 0L

    private class Session(
        var callId: String?,
        val conversationId: String?,
        val linkId: String?,
        val linkName: String?,
        val linkSecret: ByteArray?,
        val peerUserId: String,
        val peerHandle: String,
        val peerPublicKey: ByteArray?,
        val kind: String,
        val direction: String,
        var iceServers: List<IceServerDto> = emptyList(),
        var offerSdp: String? = null,
    ) {
        var pc: PeerConnection? = null
        var gathered = CompletableDeferred<Unit>()
        var closed = false
        var disconnectJob: Job? = null
        val startedAt = System.currentTimeMillis()
        var connectedAt: Long? = null
        var remoteTrack: VideoTrack? = null
        /** The caller's next TURN credential renewal (TurnRenewal). */
        var renewJob: Job? = null
        /** The newest ICE restart accepted (its sentAt), so a replayed one is ignored. */
        var lastRestartAt = 0L
    }

    init {
        scope.launch { live.events.collect { handleEvent(it) } }
        scope.launch { live.state.collect { s -> _state.update { it.copy(available = s.callsEnabled) } } }
    }

    // --- Video surfaces ---

    val eglContext get() = media.eglBase.eglBaseContext

    fun attachLocal(sink: VideoSink) = media.addLocalSink(sink)
    fun detachLocal(sink: VideoSink) = media.removeLocalSink(sink)

    fun attachRemote(sink: VideoSink) {
        remoteSinks += sink
        session?.remoteTrack?.addSink(sink)
    }

    fun detachRemote(sink: VideoSink) {
        remoteSinks -= sink
        session?.remoteTrack?.removeSink(sink)
    }

    private val remoteSinks = mutableSetOf<VideoSink>()

    // --- Starting ---

    /** Calls a contact. The caller has already been granted the microphone (and camera for video). */
    fun startCall(conversation: Conversation, kind: String) {
        if (session != null || conversation.isGroup || groupCallBusy()) return
        val peer = conversation.meta
        endedJob?.cancel()
        val s = Session(
            callId = null,
            conversationId = conversation.id,
            linkId = null,
            linkName = null,
            linkSecret = null,
            peerUserId = peer.otherUserId,
            peerHandle = conversation.handle,
            peerPublicKey = peer.otherPublicKey,
            kind = kind,
            direction = "outgoing",
        )
        begin(s, CallView(conversation.id, null, null, peer.otherUserId, conversation.handle, conversation.avatarUrl, kind, "outgoing", CallPhase.Outgoing))
        scope.launch { runOutgoing(s) { apiCall(ApiClient.json) { api.start(StartCallRequest(conversation.id, kind)) } } }
    }

    /** Joins someone's call link from its URL (…?join=<id>#<secret>). Returns an error to show, or null. */
    fun joinLink(text: String): String? {
        if (session != null || groupCallBusy()) return "You're already on a call."
        val url = text.trim()
        val id = Regex("[?&]join=([A-Za-z0-9_-]{12})").find(url)?.groupValues?.get(1)
        val fragment = url.substringAfter('#', "")
        val secret = runCatching {
            android.util.Base64.decode(fragment, android.util.Base64.URL_SAFE or android.util.Base64.NO_PADDING or android.util.Base64.NO_WRAP)
        }.getOrNull()
        if (id == null || secret == null || secret.size != CallCrypto.LINK_SECRET_BYTES) {
            return "That isn't a call link. Paste the whole link, including the part after #."
        }
        scope.launch {
            try {
                val info = apiCall(ApiClient.json) { api.linkInfo(id) }
                val name = try {
                    crypto.openLinkName(Base64Codec.decode(info.nameCiphertext), Base64Codec.decode(info.nameNonce), secret)
                } catch (e: Exception) {
                    return@launch _state.update { it.copy(linkError = "That link's secret doesn't match. Ask for the link again.") }
                }
                if (info.ownerId == identity.userId) {
                    return@launch _state.update { it.copy(linkError = "That's your own link. Share it; you'll ring when someone opens it.") }
                }
                val owner = runCatching { lookupUser(info.ownerId) }.getOrNull()
                if (session != null) return@launch
                val kind = if (info.kind == "video") "video" else "voice"
                val s = Session(null, null, info.id, name, secret, info.ownerId, owner?.username ?: "Host", null, kind, "outgoing")
                _state.update { it.copy(linkError = null) }
                begin(s, CallView(null, info.id, name, info.ownerId, s.peerHandle, owner?.avatarUrl, kind, "outgoing", CallPhase.Outgoing))
                runOutgoing(s) { apiCall(ApiClient.json) { api.joinLink(info.id) } }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                _state.update { it.copy(linkError = describe(e, "Couldn't open that link.")) }
            }
        }
        return null
    }

    private fun begin(s: Session, view: CallView) {
        session = s
        live.hold(HOLD_TAG)
        _state.update { it.copy(call = view) }
    }

    /** Shared by calling a contact and asking to join a link: media, TURN, offer. */
    private suspend fun runOutgoing(s: Session, reserve: suspend () -> StartedCallDto) {
        val cameraOn = try {
            media.start(s.kind == "video")
        } catch (e: Exception) {
            return fail(s, "Couldn't use your microphone. Allow access to call.")
        }
        if (s.closed) return
        patch { it.copy(cameraOff = !cameraOn) }
        audio.start(speaker = s.kind == "video")
        CallForegroundService.update(context, video = cameraOn, screen = false)
        warmNetwork()
        try {
            val reserved = reserve()
            if (s.closed) {
                runCatching { apiCall(ApiClient.json) { api.hangUp(reserved.callId) } }
                return
            }
            s.callId = reserved.callId
            s.iceServers = reserved.iceServers
            awaitNetwork()
            if (s.closed) return
            val pc = createPeerConnection(s)
            pc.addTransceiver(media.audioTrack, RtpTransceiver.RtpTransceiverInit(RtpTransceiver.RtpTransceiverDirection.SEND_RECV, listOf(STREAM_ID)))
            // Always a video slot, so the camera or screen sharing can start later without renegotiating.
            val camera = media.videoTrack
            if (camera != null) {
                pc.addTransceiver(camera, RtpTransceiver.RtpTransceiverInit(RtpTransceiver.RtpTransceiverDirection.SEND_RECV, listOf(STREAM_ID)))
            } else {
                pc.addTransceiver(MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO, RtpTransceiver.RtpTransceiverInit(RtpTransceiver.RtpTransceiverDirection.SEND_RECV, listOf(STREAM_ID)))
            }
            val offer = pc.createOfferAsync()
            pc.setLocalAsync(offer)
            val sdp = localSdp(s, pc)
            if (s.closed) return
            val signal = CallSignal(
                type = "offer",
                callId = reserved.callId,
                conversationId = s.linkId?.let { "link:$it" } ?: s.conversationId!!,
                from = identity.userId,
                to = s.peerUserId,
                kind = s.kind,
                sdp = sdp,
                sentAt = System.currentTimeMillis(),
            )
            apiCall(ApiClient.json) { api.offer(reserved.callId, seal(s, signal)) }
            if (!s.closed) audio.ringback()
        } catch (e: CancellationException) {
            throw e
        } catch (e: RelayUnreachable) {
            fail(s, "Couldn't reach the call relay. Try again in a minute.")
        } catch (e: Exception) {
            if (e is ApiException && e.status == 401) onSessionExpired()
            fail(s, describe(e, "Couldn't start the call. Try again."))
        }
    }

    /** Answers the ringing call. The caller has already been granted the microphone (and camera unless audio only). */
    fun accept(audioOnly: Boolean = false) {
        val s = session ?: return
        if (s.direction != "incoming" || s.callId == null || s.offerSdp == null || s.pc != null) return
        audio.stopTones()
        patch { it.copy(phase = CallPhase.Connecting) }
        val callId = s.callId!!
        scope.launch {
            val video = s.kind == "video" && !audioOnly
            val cameraOn = try {
                media.start(video)
            } catch (e: Exception) {
                return@launch fail(s, "Couldn't use your microphone. Allow access to answer.")
            }
            if (s.closed) return@launch
            patch { it.copy(cameraOff = !cameraOn, speakerOn = video) }
            audio.start(speaker = video)
            CallForegroundService.update(context, video = cameraOn, screen = false)
            try {
                warmNetwork()
                awaitNetwork()
                if (s.closed) return@launch
                val pc = createPeerConnection(s)
                pc.setRemoteAsync(SessionDescription(SessionDescription.Type.OFFER, s.offerSdp))
                for (t in pc.transceivers) {
                    t.direction = RtpTransceiver.RtpTransceiverDirection.SEND_RECV
                    val track: MediaStreamTrack? = if (t.mediaType == MediaStreamTrack.MediaType.MEDIA_TYPE_AUDIO) media.audioTrack else media.videoTrack
                    t.sender.setTrack(track, false)
                    t.sender.setStreams(listOf(STREAM_ID))
                }
                val answer = pc.createAnswerAsync()
                pc.setLocalAsync(answer)
                val sdp = localSdp(s, pc)
                if (s.closed) return@launch
                val signal = CallSignal(
                    type = "answer",
                    callId = callId,
                    conversationId = s.linkId?.let { "link:$it" } ?: s.conversationId!!,
                    from = identity.userId,
                    to = s.peerUserId,
                    kind = s.kind,
                    sdp = sdp,
                    sentAt = System.currentTimeMillis(),
                )
                apiCall(ApiClient.json) { api.answer(callId, seal(s, signal)) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: RelayUnreachable) {
                fail(s, "Couldn't reach the call relay. Try again in a minute.")
            } catch (e: Exception) {
                if (e is ApiException && e.status == 401) onSessionExpired()
                fail(s, describe(e, "Couldn't answer the call."))
            }
        }
    }

    fun decline() {
        val s = session ?: return
        if (s.direction != "incoming") return
        s.callId?.let { id -> scope.launch { runCatching { apiCall(ApiClient.json) { api.decline(id) } } } }
        finish(null, CallOutcome.declined)
    }

    /** Ends our side and tells the server (which tells the other side). */
    fun hangUp() {
        val s = session
        s?.callId?.let { id -> scope.launch { runCatching { apiCall(ApiClient.json) { api.hangUp(id) } } } }
        finish(if (s != null) "Call ended" else null)
    }

    private fun fail(s: Session, message: String) {
        if (s.closed || session !== s) return
        s.callId?.let { id -> scope.launch { runCatching { apiCall(ApiClient.json) { api.hangUp(id) } } } }
        finish(message, if (s.connectedAt != null) CallOutcome.answered else CallOutcome.failed)
    }

    /** Tears the session down and leaves a closing banner (or nothing). */
    private fun finish(message: String?, outcome: CallOutcome? = null) {
        val s = session
        session = null
        if (s != null) {
            s.closed = true
            s.disconnectJob?.cancel()
            s.renewJob?.cancel()
            s.remoteTrack?.let { track -> remoteSinks.forEach { runCatching { track.removeSink(it) } } }
            s.remoteTrack = null
            runCatching { s.pc?.dispose() }
            s.pc = null
            coolNetwork()
            audio.stop()
            media.stop()
            CallForegroundService.stop(context)
            live.release(HOLD_TAG)
            remember(s, outcome ?: when {
                s.connectedAt != null -> CallOutcome.answered
                s.direction == "incoming" -> CallOutcome.missed
                else -> CallOutcome.unanswered
            })
        }
        endedJob?.cancel()
        if (message != null) {
            _state.update { st ->
                st.copy(call = st.call?.copy(phase = CallPhase.Ended, endedMessage = message, sharingScreen = false, remoteSharing = false, remoteVideo = false))
            }
            endedJob = scope.launch {
                delay(ENDED_BANNER_MS)
                _state.update { it.copy(call = null) }
            }
        } else {
            _state.update { it.copy(call = null) }
        }
    }

    /** Hides the closing banner now. */
    fun dismiss() {
        if (session == null) {
            endedJob?.cancel()
            _state.update { it.copy(call = null) }
        }
    }

    private fun remember(s: Session, outcome: CallOutcome) {
        val id = s.callId ?: return
        val record = CallRecord(
            id = id,
            peerUserId = s.peerUserId,
            peerHandle = s.peerHandle,
            conversationId = s.conversationId,
            linkName = s.linkName,
            kind = s.kind,
            direction = s.direction,
            outcome = outcome,
            startedAt = s.startedAt,
            durationSec = s.connectedAt?.let { ((System.currentTimeMillis() - it) / 1000).toInt() } ?: 0,
        )
        val next = history.record(identity.userId, identity.privateKey, record)
        _state.update { it.copy(history = next) }
    }

    fun clearHistory() {
        history.clear(identity.userId, identity.privateKey)
        _state.update { it.copy(history = emptyList()) }
    }

    // --- In the call ---

    fun toggleMute() {
        val muted = !(state.value.call?.muted ?: return)
        media.audioTrack?.setEnabled(!muted)
        patch { it.copy(muted = muted) }
    }

    /** Camera on/off. Turning it on in a voice call starts sending video in the video slot every call has. */
    fun toggleCamera() {
        val s = session ?: return
        val call = state.value.call ?: return
        scope.launch {
            if (call.cameraOff) {
                val track = runCatching { media.startCamera() }.getOrNull()
                    ?: return@launch patch { it.copy(notice = "Couldn't turn on your camera. Allow access to use it.") }
                if (!call.sharingScreen) videoSender(s)?.setTrack(track, false)
                CallForegroundService.update(context, video = true, screen = call.sharingScreen)
                patch { it.copy(cameraOff = false, notice = null) }
            } else {
                media.stopCamera()
                if (!call.sharingScreen) videoSender(s)?.setTrack(null, false)
                patch { it.copy(cameraOff = true) }
            }
        }
    }

    fun flipCamera() = media.switchCamera()

    fun toggleSpeaker() {
        val on = !(state.value.call?.speakerOn ?: return)
        audio.setSpeaker(on)
        patch { it.copy(speakerOn = on) }
    }

    val canShareScreen: Boolean get() = tier() == "premium"

    /** Starts sharing the screen with the system's capture permission [projectionData]. Premium only (the server checks too). */
    fun startScreenShare(projectionData: Intent) {
        val s = session ?: return
        val callId = s.callId ?: return
        if (!canShareScreen) return patch { it.copy(notice = "Screen sharing is part of Premium.") }
        scope.launch {
            try {
                // The server checks the plan and tells the other side to show it.
                apiCall(ApiClient.json) { api.screen(callId, ScreenRequest(true)) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                return@launch patch { it.copy(notice = describe(e, "Couldn't share your screen.")) }
            }
            // Android 14+: the foreground service must hold the media-projection type before capture starts.
            CallForegroundService.update(context, video = state.value.call?.cameraOff == false, screen = true)
            val track = runCatching { media.startScreen(projectionData) { scope.launch { stopScreenShare() } } }.getOrNull()
            if (track == null || s.closed) {
                runCatching { apiCall(ApiClient.json) { api.screen(callId, ScreenRequest(false)) } }
                return@launch patch { it.copy(notice = "Couldn't share your screen.") }
            }
            videoSender(s)?.setTrack(track, false)
            patch { it.copy(sharingScreen = true, notice = null) }
        }
    }

    fun stopScreenShare() {
        val s = session ?: return
        if (state.value.call?.sharingScreen != true) return
        media.stopScreen()
        videoSender(s)?.setTrack(media.videoTrack, false)
        patch { it.copy(sharingScreen = false) }
        CallForegroundService.update(context, video = state.value.call?.cameraOff == false, screen = false)
        s.callId?.let { id -> scope.launch { runCatching { apiCall(ApiClient.json) { api.screen(id, ScreenRequest(false)) } } } }
    }

    fun clearNotice() = patch { it.copy(notice = null) }

    private fun videoSender(s: Session) = s.pc?.transceivers?.firstOrNull { it.mediaType == MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO }?.sender

    // --- WebRTC ---

    /** Starts [networkWarmer] (once): an offer with no relays, whose gathering starts the network monitor. */
    private suspend fun warmNetwork() {
        if (networkWarmer != null) return
        val config = PeerConnection.RTCConfiguration(emptyList()).apply {
            iceTransportsType = PeerConnection.IceTransportsType.RELAY
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
        }
        val pc = media.factory.createPeerConnection(config, PeerObserver()) ?: return
        networkWarmer = pc
        warmedAt = System.currentTimeMillis()
        try {
            pc.addTransceiver(MediaStreamTrack.MediaType.MEDIA_TYPE_AUDIO, RtpTransceiver.RtpTransceiverInit(RtpTransceiver.RtpTransceiverDirection.RECV_ONLY))
            pc.setLocalAsync(pc.createOfferAsync())
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // Best effort: without it a call works as before, just without the head start.
        }
    }

    /** Gives the monitor [NETWORK_WARMUP_MS] since [warmNetwork] to report the phone's networks. */
    private suspend fun awaitNetwork() {
        val waited = System.currentTimeMillis() - warmedAt
        if (waited < NETWORK_WARMUP_MS) delay(NETWORK_WARMUP_MS - waited)
    }

    private fun coolNetwork() {
        networkWarmer?.let { runCatching { it.dispose() } }
        networkWarmer = null
    }

    private fun rtcConfig(iceServers: List<IceServerDto>): PeerConnection.RTCConfiguration {
        val servers = iceServers.map { PeerConnection.IceServer.builder(it.urls).setUsername(it.username).setPassword(it.credential).createIceServer() }
        return PeerConnection.RTCConfiguration(servers).apply {
            // Relay only: never reveal either person's IP address to the other.
            iceTransportsType = PeerConnection.IceTransportsType.RELAY
            bundlePolicy = PeerConnection.BundlePolicy.MAXBUNDLE
            rtcpMuxPolicy = PeerConnection.RtcpMuxPolicy.REQUIRE
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
            continualGatheringPolicy = PeerConnection.ContinualGatheringPolicy.GATHER_ONCE
        }
    }

    private fun createPeerConnection(s: Session): PeerConnection {
        val config = rtcConfig(s.iceServers)
        val observer = object : PeerObserver() {
            override fun onIceGatheringChange(state: PeerConnection.IceGatheringState) {
                if (state == PeerConnection.IceGatheringState.COMPLETE) s.gathered.complete(Unit)
            }

            override fun onConnectionChange(newState: PeerConnection.PeerConnectionState) {
                scope.launch { connectionChanged(s, newState) }
            }

            override fun onTrack(transceiver: RtpTransceiver) {
                val track = transceiver.receiver.track() as? VideoTrack ?: return
                scope.launch {
                    if (s.closed) return@launch
                    s.remoteTrack = track
                    remoteSinks.forEach { track.addSink(it) }
                    track.addSink(remoteFrameWatcher(s))
                }
            }
        }
        val pc = media.factory.createPeerConnection(config, observer) ?: throw IllegalStateException("Couldn't set up the call")
        s.pc = pc
        return pc
    }

    /** Notices when the other side's video actually starts (so a voice call can show it) without touching frames. */
    private fun remoteFrameWatcher(s: Session) = object : VideoSink {
        @Volatile private var seen = false
        override fun onFrame(frame: org.webrtc.VideoFrame) {
            if (seen) return
            seen = true
            scope.launch { if (session === s) patch { it.copy(remoteVideo = true) } }
        }
    }

    private fun connectionChanged(s: Session, newState: PeerConnection.PeerConnectionState) {
        if (s.closed || session !== s) return
        when (newState) {
            PeerConnection.PeerConnectionState.CONNECTED -> {
                audio.stopTones()
                s.disconnectJob?.cancel()
                s.disconnectJob = null
                if (s.connectedAt == null) s.connectedAt = System.currentTimeMillis()
                // The caller keeps the TURN credentials fresh for a long call (only one side restarts ICE).
                if (s.direction == "outgoing" && s.renewJob == null) scheduleRenewal(s)
                val pc = s.pc
                val mine = CallCrypto.sdpFingerprint(pc?.localDescription?.description)
                val theirs = CallCrypto.sdpFingerprint(pc?.remoteDescription?.description)
                patch { it.copy(phase = CallPhase.Active, connectedAt = s.connectedAt, sas = if (mine != null && theirs != null) crypto.sasWords(mine, theirs) else it.sas) }
            }
            PeerConnection.PeerConnectionState.DISCONNECTED -> {
                patch { it.copy(phase = CallPhase.Reconnecting) }
                if (s.disconnectJob == null) {
                    s.disconnectJob = scope.launch {
                        delay(RECONNECT_GRACE_MS)
                        fail(s, "Call dropped")
                    }
                }
            }
            PeerConnection.PeerConnectionState.FAILED -> fail(s, "Call dropped")
            else -> Unit
        }
    }

    // --- Long calls: fresh TURN credentials and an ICE restart before the old ones expire ---

    private fun scheduleRenewal(s: Session) {
        s.renewJob?.cancel()
        val expiry = TurnRenewal.expiryMs(s.iceServers) ?: return
        s.renewJob = scope.launch {
            delay(TurnRenewal.delayMs(expiry))
            renewRelay(s)
        }
    }

    private suspend fun renewRelay(s: Session) {
        val pc = s.pc ?: return
        val callId = s.callId ?: return
        if (s.closed) return
        try {
            val fresh = apiCall(ApiClient.json) { api.renewRelay(callId) }
            if (s.closed) return
            s.iceServers = fresh.iceServers
            pc.setConfiguration(rtcConfig(fresh.iceServers))
            s.gathered = CompletableDeferred()
            pc.setLocalAsync(pc.createOfferAsync(iceRestart = true))
            val sdp = localSdp(s, pc)
            if (s.closed) return
            apiCall(ApiClient.json) { api.signal(callId, seal(s, restartSignal(s, callId, "restart-offer", sdp))) }
            scheduleRenewal(s)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // The current relay keeps working until its credentials lapse: try again in a minute.
            if (!s.closed) s.renewJob = scope.launch {
                delay(60_000)
                renewRelay(s)
            }
        }
    }

    private fun restartSignal(s: Session, callId: String, type: String, sdp: String) = CallSignal(
        type = type,
        callId = callId,
        conversationId = s.linkId?.let { "link:$it" } ?: s.conversationId!!,
        from = identity.userId,
        to = s.peerUserId,
        kind = s.kind,
        sdp = sdp,
        sentAt = System.currentTimeMillis(),
    )

    /** An ICE restart from the other side (callee: their restart offer; caller: the answer to ours). */
    private suspend fun handleRestart(s: Session, sealed: SealedSignal) {
        val pc = s.pc ?: return
        val callId = s.callId ?: return
        val type = if (s.direction == "incoming") "restart-offer" else "restart-answer"
        val signal = try {
            val expected = CallCrypto.Expected(type, callId, s.conversationId.orEmpty(), s.peerUserId, identity.userId, s.kind)
            if (s.linkSecret != null) crypto.openForLink(sealed, s.linkSecret, s.linkId!!, expected)
            else crypto.open(sealed, s.peerPublicKey!!, identity.privateKey, expected)
        } catch (e: Exception) {
            return // not from them, not for this call, or too old: ignored
        }
        if (signal.sentAt <= s.lastRestartAt) return
        // The DTLS identity (what the safety words are made of) must not change mid-call.
        if (CallCrypto.sdpFingerprint(signal.sdp) != CallCrypto.sdpFingerprint(pc.remoteDescription?.description)) return
        s.lastRestartAt = signal.sentAt
        try {
            if (type == "restart-answer") {
                pc.setRemoteAsync(SessionDescription(SessionDescription.Type.ANSWER, signal.sdp))
                return
            }
            // This side gets fresh credentials too, so both relays' allocations outlive the old ones.
            val fresh = apiCall(ApiClient.json) { api.renewRelay(callId) }
            s.iceServers = fresh.iceServers
            pc.setConfiguration(rtcConfig(fresh.iceServers))
            s.gathered = CompletableDeferred()
            pc.setRemoteAsync(SessionDescription(SessionDescription.Type.OFFER, signal.sdp))
            pc.setLocalAsync(pc.createAnswerAsync())
            val sdp = localSdp(s, pc)
            if (s.closed) return
            apiCall(ApiClient.json) { api.signal(callId, seal(s, restartSignal(s, callId, "restart-answer", sdp))) }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // The call carries on over the current relay; the caller tries again later.
        }
    }

    /** Our half of the SDP, once every relay candidate is in it. */
    private suspend fun localSdp(s: Session, pc: PeerConnection): String {
        withTimeoutOrNull(GATHER_TIMEOUT_MS) { s.gathered.await() }
        val sdp = pc.localDescription?.description.orEmpty()
        if (!sdp.contains(" typ relay ")) throw RelayUnreachable()
        return sdp
    }

    private fun seal(s: Session, signal: CallSignal): SealedSignalDto {
        val sealed = if (s.linkSecret != null) crypto.sealForLink(signal, s.linkSecret) else crypto.seal(signal, s.peerPublicKey!!, identity.privateKey)
        return SealedSignalDto(Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce))
    }

    // --- Live events ---

    private suspend fun handleEvent(event: JsonObject) {
        val type = event.str("type")
        val s = session
        if (type == "incoming") {
            if (s != null || groupCallBusy()) return // the server doesn't ring someone who's on a call
            incoming(event)
            return
        }
        if (s == null || s.callId != event.str("callId")) return
        when (type) {
            "answered" -> {
                if (s.direction != "outgoing" || s.pc == null) return
                audio.stopTones()
                patch { it.copy(phase = CallPhase.Connecting) }
                try {
                    val sealed = sealedOf(event["answer"]) ?: throw CallSignalException("Malformed call setup.")
                    val expected = CallCrypto.Expected("answer", s.callId!!, s.conversationId.orEmpty(), s.peerUserId, identity.userId, s.kind)
                    val answer = if (s.linkSecret != null) crypto.openForLink(sealed, s.linkSecret, s.linkId!!, expected)
                    else crypto.open(sealed, s.peerPublicKey!!, identity.privateKey, expected)
                    s.pc!!.setRemoteAsync(SessionDescription(SessionDescription.Type.ANSWER, answer.sdp))
                } catch (e: CallSignalException) {
                    fail(s, "Hung up: ${e.message}")
                } catch (e: Exception) {
                    fail(s, "Couldn't connect the call.")
                }
            }
            "signal" -> handleRestart(s, sealedOf(event["signal"]) ?: return)
            "screen" -> patch { it.copy(remoteSharing = (event["on"] as? JsonPrimitive)?.content == "true") }
            "ended" -> {
                val reason = event.str("reason")
                val incoming = s.direction == "incoming"
                val message = when (reason) {
                    "declined" -> "Call declined"
                    "missed" -> if (incoming) "Missed call" else "No answer"
                    "busy" -> if (incoming) "Call ended" else "They're on another call"
                    "failed" -> "Call dropped"
                    else -> "Call ended"
                }
                val outcome = when (reason) {
                    "declined" -> CallOutcome.declined
                    "missed" -> if (incoming) CallOutcome.missed else CallOutcome.unanswered
                    else -> null
                }
                finish(message, outcome)
            }
        }
    }

    private suspend fun incoming(event: JsonObject) {
        val callId = event.str("callId")
        val callerId = event.str("callerId")
        val kind = if (event.str("kind") == "video") "video" else "voice"
        val sealed = sealedOf(event["offer"]) ?: return
        val iceServers = (event["iceServers"] as? JsonArray)?.mapNotNull {
            runCatching { ApiClient.json.decodeFromJsonElement<IceServerDto>(it) }.getOrNull()
        }.orEmpty()
        endedJob?.cancel()

        val linkId = event.str("linkId")
        if (linkId.isNotEmpty()) {
            // Someone opened one of your call links.
            if (!state.value.linksLoaded) refreshLinksNow()
            val link = state.value.links.firstOrNull { it.id == linkId }
            val guest = runCatching { lookupUser(callerId) }.getOrNull()
            val view = CallView(null, linkId, link?.name, callerId, guest?.username ?: "Someone", guest?.avatarUrl, kind, "incoming", CallPhase.Incoming)
            if (link == null || session != null) return
            val offer = try {
                crypto.openForLink(sealed, link.secret, link.id, CallCrypto.Expected("offer", callId, "", callerId, identity.userId))
            } catch (e: CallSignalException) {
                runCatching { apiCall(ApiClient.json) { api.decline(callId) } }
                return showBlocked(view, "Blocked a request to join \"${link.name}\" that wasn't made with the link.")
            }
            ring(Session(callId, null, link.id, link.name, link.secret, callerId, view.peerHandle, null, kind, "incoming", iceServers, offer.sdp), view)
            return
        }

        val conversationId = event.str("conversationId")
        val contact = findContact(conversationId, callerId) ?: return
        if (session != null) return
        val view = CallView(contact.id, null, null, callerId, contact.handle, contact.avatarUrl, kind, "incoming", CallPhase.Incoming)
        val offer = try {
            crypto.open(sealed, contact.meta.otherPublicKey, identity.privateKey, CallCrypto.Expected("offer", callId, contact.id, callerId, identity.userId, kind))
        } catch (e: CallSignalException) {
            // Not sealed by this contact's key: never ring for it.
            runCatching { apiCall(ApiClient.json) { api.decline(callId) } }
            return showBlocked(view, "Blocked a call that couldn't be verified as coming from ${contact.handle}'s key.")
        }
        ring(Session(callId, contact.id, null, null, null, callerId, contact.handle, contact.meta.otherPublicKey, kind, "incoming", iceServers, offer.sdp), view)
    }

    private fun ring(s: Session, view: CallView) {
        begin(s, view)
        // Muted contacts' calls show, but don't ring.
        // Muted contacts' calls show but don't ring; with the ringtone off, no call rings.
        if (ringtone() && s.peerUserId !in live.state.value.muted) audio.ring()
        CallNotifications.showIncoming(context, view.peerHandle, view.kind == "video")
    }

    private fun showBlocked(view: CallView, message: String) {
        _state.update { it.copy(call = view.copy(phase = CallPhase.Ended, endedMessage = message)) }
        endedJob?.cancel()
        endedJob = scope.launch {
            delay(ENDED_BANNER_MS * 2)
            _state.update { it.copy(call = null) }
        }
    }

    /** The 1:1 chat a call belongs to; right after sign-in the chat list may still be loading. */
    private suspend fun findContact(conversationId: String, peerUserId: String): Conversation? {
        repeat(20) {
            conversations().firstOrNull { !it.isGroup && it.id == conversationId && it.meta.otherUserId == peerUserId }?.let { return it }
            delay(300)
        }
        return null
    }

    /**
     * Settings' relay check: how long one TURN relay takes to hand out an
     * address. Only asks for the address (no media, no identity); null when
     * none came back.
     */
    suspend fun probeRelay(server: IceServerDto, url: String): Long? {
        warmNetwork()
        awaitNetwork()
        val started = System.currentTimeMillis()
        val gathered = CompletableDeferred<Unit>()
        val config = PeerConnection.RTCConfiguration(
            listOf(PeerConnection.IceServer.builder(url).setUsername(server.username).setPassword(server.credential).createIceServer()),
        ).apply {
            iceTransportsType = PeerConnection.IceTransportsType.RELAY
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
        }
        val pc = media.factory.createPeerConnection(config, object : PeerObserver() {
            override fun onIceGatheringChange(state: PeerConnection.IceGatheringState) {
                if (state == PeerConnection.IceGatheringState.COMPLETE) gathered.complete(Unit)
            }
        })
        if (pc == null) {
            if (session == null) coolNetwork()
            return null
        }
        return try {
            pc.addTransceiver(MediaStreamTrack.MediaType.MEDIA_TYPE_AUDIO, RtpTransceiver.RtpTransceiverInit(RtpTransceiver.RtpTransceiverDirection.RECV_ONLY))
            pc.setLocalAsync(pc.createOfferAsync())
            withTimeoutOrNull(GATHER_TIMEOUT_MS) { gathered.await() }
            if (pc.localDescription?.description.orEmpty().contains(" typ relay ")) System.currentTimeMillis() - started else null
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            null
        } finally {
            pc.dispose()
            if (session == null) coolNetwork()
        }
    }

    // --- Call links ---

    fun refreshLinks() {
        scope.launch { refreshLinksNow() }
    }

    private suspend fun refreshLinksNow() {
        try {
            val mine = apiCall(ApiClient.json) { api.links() }.mapNotNull { dto ->
                runCatching {
                    val secret = crypto.openOwnedSecret(Base64Codec.decode(dto.secretSealed), identity.publicKey, identity.privateKey)
                    CallLink(
                        id = dto.id,
                        kind = dto.kind,
                        name = crypto.openLinkName(Base64Codec.decode(dto.nameCiphertext), Base64Codec.decode(dto.nameNonce), secret),
                        url = crypto.linkUrl(webAppBase, dto.id, secret),
                        createdOn = dto.createdOn,
                        secret = secret,
                    )
                }.getOrNull()
            }
            _state.update { it.copy(links = mine, linksLoaded = true) }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (e is ApiException && e.status == 401) onSessionExpired()
            _state.update { it.copy(linksLoaded = true) }
        }
    }

    fun createLink(nameIn: String, kindIn: String) {
        val name = nameIn.trim().ifEmpty { "Call link" }.take(60)
        val kind = if (kindIn == "video") "video" else "voice"
        val rank = when (tier()) { "premium" -> 2; "pro" -> 1; else -> 0 }
        if (rank < if (kind == "video") 2 else 1) {
            return _state.update { it.copy(linkError = if (kind == "video") "Video call links are part of Premium." else "Call links are part of Pro and Premium.") }
        }
        scope.launch {
            try {
                val secret = crypto.newLinkSecret()
                val sealedName = crypto.sealLinkName(name, secret)
                val dto = apiCall(ApiClient.json) {
                    api.createLink(
                        CreateCallLinkRequest(
                            kind,
                            Base64Codec.encode(crypto.sealSecretForOwner(secret, identity.publicKey)),
                            Base64Codec.encode(sealedName.ciphertext),
                            Base64Codec.encode(sealedName.nonce),
                        ),
                    )
                }
                _state.update { it.copy(linkError = null, createdLinkUrl = crypto.linkUrl(webAppBase, dto.id, secret)) }
                refreshLinksNow()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                _state.update { it.copy(linkError = describe(e, "Couldn't create the link.")) }
            }
        }
    }

    fun deleteLink(id: String) {
        scope.launch {
            try {
                apiCall(ApiClient.json) { api.deleteLink(id) }
                _state.update { s -> s.copy(links = s.links.filterNot { it.id == id }) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                _state.update { it.copy(linkError = describe(e, "Couldn't delete the link.")) }
            }
        }
    }

    fun clearLinkMessages() = _state.update { it.copy(linkError = null, createdLinkUrl = null) }

    // --- Helpers ---

    private fun patch(change: (CallView) -> CallView) = _state.update { s -> s.copy(call = s.call?.let(change)) }

    private fun describe(e: Exception, fallback: String): String = when (e) {
        is ApiException -> e.message ?: fallback
        is CallSignalException -> e.message ?: fallback
        else -> fallback
    }

    private fun sealedOf(element: kotlinx.serialization.json.JsonElement?): SealedSignal? {
        val o = element as? JsonObject ?: return null
        return runCatching { SealedSignal(Base64Codec.decode(o.str("ciphertext")), Base64Codec.decode(o.str("nonce"))) }.getOrNull()
    }

    private fun JsonObject.str(key: String): String = (this[key] as? JsonPrimitive)?.takeIf { it.isString }?.content.orEmpty()

    /** Signing out ends any call. */
    fun close() {
        if (session != null) hangUp()
        scope.cancel()
        coolNetwork()
        media.release()
    }

    private class RelayUnreachable : Exception()

    companion object {
        private const val HOLD_TAG = "call"
        private const val STREAM_ID = "redvault"
        /** How long ICE gathering may take; relay candidates normally arrive well within this. */
        const val GATHER_TIMEOUT_MS = 6000L
        /** How long [networkWarmer] runs before a call gathers. The monitor reports networks within ~20 ms. */
        const val NETWORK_WARMUP_MS = 250L
        /** A dropped connection gets this long to come back before the call ends. */
        const val RECONNECT_GRACE_MS = 15_000L
        const val ENDED_BANNER_MS = 4000L
    }
}

// --- WebRTC callback adapters ---

private suspend fun PeerConnection.createOfferAsync(iceRestart: Boolean = false): SessionDescription = suspendCancellableCoroutine { cont ->
    createOffer(object : SdpAdapter() {
        override fun onCreateSuccess(sdp: SessionDescription) = cont.resume(sdp)
        override fun onCreateFailure(error: String?) = cont.resumeWithException(IllegalStateException(error ?: "createOffer failed"))
    }, MediaConstraints().apply { if (iceRestart) mandatory.add(MediaConstraints.KeyValuePair("IceRestart", "true")) })
}

private suspend fun PeerConnection.createAnswerAsync(): SessionDescription = suspendCancellableCoroutine { cont ->
    createAnswer(object : SdpAdapter() {
        override fun onCreateSuccess(sdp: SessionDescription) = cont.resume(sdp)
        override fun onCreateFailure(error: String?) = cont.resumeWithException(IllegalStateException(error ?: "createAnswer failed"))
    }, MediaConstraints())
}

private suspend fun PeerConnection.setLocalAsync(sdp: SessionDescription): Unit = suspendCancellableCoroutine { cont ->
    setLocalDescription(object : SdpAdapter() {
        override fun onSetSuccess() = cont.resume(Unit)
        override fun onSetFailure(error: String?) = cont.resumeWithException(IllegalStateException(error ?: "setLocalDescription failed"))
    }, sdp)
}

private suspend fun PeerConnection.setRemoteAsync(sdp: SessionDescription): Unit = suspendCancellableCoroutine { cont ->
    setRemoteDescription(object : SdpAdapter() {
        override fun onSetSuccess() = cont.resume(Unit)
        override fun onSetFailure(error: String?) = cont.resumeWithException(IllegalStateException(error ?: "setRemoteDescription failed"))
    }, sdp)
}

private open class SdpAdapter : SdpObserver {
    override fun onCreateSuccess(sdp: SessionDescription) {}
    override fun onSetSuccess() {}
    override fun onCreateFailure(error: String?) {}
    override fun onSetFailure(error: String?) {}
}

private open class PeerObserver : PeerConnection.Observer {
    override fun onSignalingChange(state: PeerConnection.SignalingState) {}
    override fun onIceConnectionChange(state: PeerConnection.IceConnectionState) {}
    override fun onIceConnectionReceivingChange(receiving: Boolean) {}
    override fun onIceGatheringChange(state: PeerConnection.IceGatheringState) {}
    override fun onIceCandidate(candidate: org.webrtc.IceCandidate) {}
    override fun onIceCandidatesRemoved(candidates: Array<out org.webrtc.IceCandidate>) {}
    override fun onAddStream(stream: org.webrtc.MediaStream) {}
    override fun onRemoveStream(stream: org.webrtc.MediaStream) {}
    override fun onDataChannel(channel: org.webrtc.DataChannel) {}
    override fun onRenegotiationNeeded() {}
}

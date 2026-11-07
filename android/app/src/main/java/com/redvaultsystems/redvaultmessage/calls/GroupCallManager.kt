package com.redvaultsystems.redvaultmessage.calls

import android.content.Context
import android.content.Intent
import android.os.SystemClock
import android.util.Log
import com.redvaultsystems.redvaultmessage.BuildConfig
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.ExpectedRoomKey
import com.redvaultsystems.redvaultmessage.crypto.GroupCallCrypto
import com.redvaultsystems.redvaultmessage.crypto.GroupKeyException
import com.redvaultsystems.redvaultmessage.crypto.SealedSignal
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.CallApi
import com.redvaultsystems.redvaultmessage.data.api.GroupCallJoinDto
import com.redvaultsystems.redvaultmessage.data.api.GroupKeyEntry
import com.redvaultsystems.redvaultmessage.data.api.SealedSignalDto
import com.redvaultsystems.redvaultmessage.data.api.SendGroupKeysRequest
import com.redvaultsystems.redvaultmessage.data.api.StartGroupCallRequest
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import com.redvaultsystems.redvaultmessage.ui.live.LiveSession
import io.livekit.android.ConnectOptions
import io.livekit.android.LiveKit
import io.livekit.android.LiveKitOverrides
import io.livekit.android.AudioOptions
import io.livekit.android.RoomOptions
import io.livekit.android.audio.NoAudioHandler
import io.livekit.android.e2ee.E2EEOptions
import io.livekit.android.events.RoomEvent
import io.livekit.android.room.Room
import io.livekit.android.room.participant.LocalParticipant
import io.livekit.android.room.participant.Participant
import io.livekit.android.room.track.CameraPosition
import io.livekit.android.room.track.LocalAudioTrackOptions
import io.livekit.android.room.track.LocalTrackPublication
import io.livekit.android.room.track.LocalVideoTrack
import io.livekit.android.room.track.LocalVideoTrackOptions
import io.livekit.android.room.track.Track
import io.livekit.android.room.track.VideoPreset169
import io.livekit.android.renderer.TextureViewRenderer
import kotlinx.coroutines.CancellationException
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
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import okhttp3.OkHttpClient
import livekit.org.webrtc.PeerConnection
import okhttp3.HttpUrl.Companion.toHttpUrl

enum class GroupPhase { Incoming, Connecting, Active, Reconnecting, Ended }

/** One video tile: a person's camera (or their picture when it's off), or a shared screen. */
data class GroupTile(
    val key: String,
    val userId: String?,
    val handle: String,
    val avatarUrl: String?,
    val isSelf: Boolean,
    val screen: Boolean,
    val video: io.livekit.android.room.track.VideoTrack?,
    val micMuted: Boolean,
    val speaking: Boolean,
    /** Your own front camera is shown mirrored, like a mirror. */
    val mirrored: Boolean = false,
)

/** What the group call screens show. */
data class GroupCallView(
    val callId: String?,
    val conversationId: String,
    val groupLabel: String,
    /** "voice" or "video". */
    val kind: String,
    val phase: GroupPhase,
    /** Who started it (shown when it rings). */
    val hostHandle: String?,
    val muted: Boolean = false,
    val cameraOff: Boolean = true,
    val speakerOn: Boolean = false,
    val sharingScreen: Boolean = false,
    /** Which camera is on (flipping changes it). */
    val frontCamera: Boolean = true,
    val connectedAt: Long? = null,
    val tiles: List<GroupTile> = emptyList(),
    /** Four words from the call and every participant's identity key. */
    val sas: List<String>? = null,
    /** This phone has the room key for the current epoch (until then nothing is sent or shown). */
    val keyReady: Boolean = false,
    /** A problem worth showing without ending the call (e.g. a key that failed to verify). */
    val notice: String? = null,
    val endedMessage: String? = null,
)

/** A call going on in one of your groups, for the Join banner. */
data class ActiveGroupCall(val callId: String, val kind: String, val participants: Int)

data class GroupCallsState(
    val call: GroupCallView? = null,
    /** The server has LiveKit configured (from the live channel). */
    val available: Boolean = false,
    /** Calls going on in your groups, by conversation id. */
    val active: Map<String, ActiveGroupCall> = emptyMap(),
)

/**
 * Group voice and video calls with screen sharing, in group chats: a port of
 * the web's state/GroupCallContext.tsx on LiveKit's Android SDK.
 *
 *   - Media: WebRTC through a LiveKit SFU on this app's own server, which
 *     forwards each person's media to the others. Every frame is also
 *     encrypted end to end on this phone with the call's room key
 *     (crypto/GroupCallCrypto.kt, [RoomKeyProvider]), so the SFU only ever
 *     forwards ciphertext. Nothing is published until this phone holds a room
 *     key and the room's E2EE is on, and a track starts muted until its frames
 *     are being encrypted. If a track can't be moved to a new key the call ends
 *     rather than send without encryption (GroupCallGuards.kt has why).
 *   - Keys: the first person in the call makes a room key per epoch and seals
 *     it to each participant's identity key; the server moves to a new epoch
 *     whenever someone joins or leaves.
 *   - Safety words come from every participant's identity key.
 *   - Plans: only a Premium member can start a call (voice or video); anyone
 *     in the group can join; screen sharing needs your own Premium. The server
 *     enforces each, down to what the LiveKit token lets you publish.
 *   - Link: once the starter is connected, their app posts the call's link in
 *     the group chat ([GroupCallLink]). Members paste it into "Join with a
 *     link" ([joinByLink]) or tap it; it only works while the call is on.
 *   - Audio routing is [CallAudio]'s, as for one-to-one calls.
 */
class GroupCallManager(
    private val context: Context,
    private val api: CallApi,
    private val crypto: GroupCallCrypto,
    private val identity: SelfIdentity,
    private val live: LiveSession,
    /** The web app's address (links to a call open there). */
    private val webAppBase: String,
    /** The API's address; LiveKit's signaling path is on the same host. */
    private val apiBase: String,
    /** The HTTP client for LiveKit's signaling: no cookies, no identifying headers, our own host only. */
    /** An OkHttp client allowed to reach only the given call-server host. */
    private val signalingClient: (host: String) -> OkHttpClient,
    /** The chats loaded so far (a group's members and their identity keys come from here). */
    private val conversations: () -> List<Conversation>,
    private val tier: () -> String,
    /** Posts [text] in the chat as an ordinary end-to-end encrypted message. */
    private val sendChatMessage: (conversationId: String, text: String, onResult: (Boolean) -> Unit) -> Unit,
    /** A one-to-one call is on. */
    private val oneToOneBusy: () -> Boolean,
    private val ringtone: () -> Boolean = { true },
    private val echoCancellation: () -> Boolean = { true },
    private val onSessionExpired: () -> Unit,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow(GroupCallsState())
    val state: StateFlow<GroupCallsState> = _state.asStateFlow()

    private val audio by lazy { CallAudio(context) }
    private var session: Session? = null
    private var endedJob: Job? = null

    /** Room events and room keys are handled one at a time, in the order they arrive. */
    private val callMutex = Mutex()

    /** Calls the server took this account out of: a join still under way when the news came doesn't go through. */
    private val removedFrom = HashSet<String>()

    private class Session(var callId: String?, val conversationId: String, var kind: String) {
        var room: Room? = null
        /** Made when the call connects: it needs WebRTC's native library, which isn't loaded while a call only rings. */
        val keysLazy = lazy { RoomKeyProvider() }
        val keys: RoomKeyProvider get() = keysLazy.value
        /** Roster and key events for this call that arrived before the room was connected. */
        val buffer = mutableListOf<JsonObject>()
        var ready = false
        var epoch = 0L
        var keyholder: String? = null
        var members: List<RosterMember> = emptyList()
        /** The epoch whose key this phone holds (0: none yet). */
        var keyEpoch = 0L
        /** Keys that arrived for an epoch this phone hasn't seen the roster of yet. */
        val earlyKeys = mutableMapOf<Long, JsonObject>()
        var published = false
        var closed = false
        /** This phone started the call: post its link in the chat once connected. */
        var announce = false
        var ringing = false
        /** The key slot the senders' encryption was last set up for. */
        var cryptorSlot = -1
        var eventsJob: Job? = null
        /** libwebrtc drops what it can't encrypt, so this is said at once. */
        val sendFailures = TrackFailures(0)
        /** A frame can beat its key here, so this waits to see if it passes. */
        val receiveFailures = TrackFailures(E2EE_GRACE_MS)
        var e2eeCheckJob: Job? = null
    }

    private data class RosterMember(val userId: String, val identity: String)

    init {
        scope.launch { live.events.collect { handleEvent(it) } }
        scope.launch {
            live.state.collect { s ->
                val wasAvailable = _state.value.available
                _state.update { it.copy(available = s.groupCallsEnabled) }
                if (s.groupCallsEnabled && !wasAvailable) refreshActive()
            }
        }
    }

    // --- Starting and joining ---

    /** Whether this account may start a call: Premium. Anyone may join one that's going on. */
    val canStart: Boolean get() = tier() == "premium"

    /** True while a group call (not just its closing banner) is on or ringing. */
    fun isBusy(): Boolean = session != null

    /**
     * Starts a call in [conversation] (a group), or joins the one already going on there. The caller has
     * already been granted the microphone (and the camera for video). Returns something to show, or null.
     */
    fun start(conversation: Conversation, kind: String): String? {
        if (!conversation.isGroup) return "Group calls are for groups."
        if (conversation.meta.community != null) return COMMUNITY_CALLS
        if (!_state.value.available) return NOT_AVAILABLE
        if (session != null || oneToOneBusy()) return ALREADY_IN_CALL
        val ongoing = _state.value.active[conversation.id]
        if (ongoing == null && !canStart) return PREMIUM_ONLY
        endedJob?.cancel()
        val s = Session(null, conversation.id, ongoing?.kind ?: kind)
        begin(s, blankView(conversation, s.kind, GroupPhase.Connecting, null, null))
        scope.launch {
            val join = try {
                apiCall(ApiClient.json) {
                    if (ongoing != null) api.joinGroupCall(ongoing.callId) else api.startGroupCall(StartGroupCallRequest(conversation.id, kind))
                }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                if (!s.closed) finish(s, describe(e, "Couldn't start the call. Try again."))
                return@launch
            }
            if (s.closed) {
                leaveQuietly(join.callId)
                return@launch
            }
            s.announce = join.created
            connect(s, join)
        }
        return null
    }

    /**
     * Joins the call a pasted link points to. [onResult] gets null once the call screen is up, or something
     * to show if the call isn't on, you aren't in its group, or you're already in a call.
     */
    fun joinByLink(callId: String, onResult: (String?) -> Unit) {
        if (!_state.value.available) return onResult(NOT_AVAILABLE)
        // The call is ringing here already: pasting its link is the same as answering.
        val ringing = session
        if (ringing != null && ringing.ringing && ringing.callId == callId) {
            accept()
            return onResult(null)
        }
        if (session != null || oneToOneBusy()) return onResult(ALREADY_IN_CALL)
        scope.launch {
            // The server answers "This call has ended." for a call that was never started, one that's over,
            // and one in a group you're not in.
            val join = try {
                apiCall(ApiClient.json) { api.joinGroupCall(callId) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                return@launch onResult(describe(e, "Couldn't join the call. Try again."))
            }
            val group = groupOf(join.conversationId)
            if (group == null) {
                leaveQuietly(join.callId)
                val inCommunity = conversations().any { it.id == join.conversationId && it.meta.community != null }
                return@launch onResult(
                    if (inCommunity) COMMUNITY_CALLS else "That call is in a group chat that isn't loaded yet. Open your chats and try the link again.",
                )
            }
            if (session != null) {
                leaveQuietly(join.callId)
                return@launch onResult(ALREADY_IN_CALL)
            }
            endedJob?.cancel()
            val s = Session(join.callId, group.id, join.kind)
            begin(s, blankView(group, join.kind, GroupPhase.Connecting, join.callId, null))
            onResult(null)
            // Connecting carries on behind the call screen; it reports its own failures there.
            connect(s, join)
        }
    }

    /** Answers the ringing call. The caller has already been granted the microphone (and the camera for video). */
    fun accept() {
        val s = session ?: return
        val callId = s.callId
        if (!s.ringing || callId == null || s.room != null) return
        audio.stopTones()
        s.ringing = false
        patch { it.copy(phase = GroupPhase.Connecting) }
        // Ringing is over: a late "stop ringing" mustn't cancel the join.
        s.callId = null
        scope.launch {
            try {
                val join = apiCall(ApiClient.json) { api.joinGroupCall(callId) }
                if (s.closed) return@launch leaveQuietly(callId)
                connect(s, join)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                if (!s.closed) finish(s, describe(e, "Couldn't join the call."))
            }
        }
    }

    fun decline() {
        val s = session ?: return
        if (s.room != null || !s.ringing) return
        s.callId?.let { id -> scope.launch { runCatching { apiCall(ApiClient.json) { api.declineGroupCall(id) } } } }
        finish(s, null)
    }

    /** Leaves the call (the others get a new key straight away). */
    fun leave() {
        val s = session ?: return
        s.callId?.let { leaveQuietly(it) }
        finish(s, "You left the call")
    }

    private fun leaveQuietly(callId: String) {
        scope.launch { runCatching { apiCall(ApiClient.json) { api.leaveGroupCall(callId) } } }
    }

    private fun begin(s: Session, view: GroupCallView) {
        session = s
        live.hold(HOLD_TAG)
        _state.update { it.copy(call = view) }
    }

    /** Hides the closing banner now. */
    fun dismiss() {
        if (session == null) {
            endedJob?.cancel()
            _state.update { it.copy(call = null) }
        }
    }

    fun clearNotice() = patch { it.copy(notice = null) }

    /** Signing out ends any call. */
    fun close() {
        session?.let { s ->
            // The scope is about to be cancelled, so the others hear we left from a scope that outlives it.
            s.callId?.let { id ->
                CoroutineScope(SupervisorJob() + Dispatchers.IO).launch { runCatching { apiCall(ApiClient.json) { api.leaveGroupCall(id) } } }
            }
            finish(s, null)
        }
        scope.cancel()
    }

    // --- Connecting ---

    /** Connects to LiveKit with end-to-end encryption on, then catches up on the roster and keys. */
    private suspend fun connect(s: Session, join: GroupCallJoinDto) {
        s.callId = join.callId
        s.kind = join.kind
        if (join.callId in removedFrom) return finish(s, REMOVED_FROM_CALL)
        patch { it.copy(callId = join.callId, kind = join.kind, cameraOff = join.kind != "video") }
        val echo = echoCancellation()
        audio.start(speaker = join.kind == "video")
        patch { it.copy(speakerOn = join.kind == "video") }
        CallForegroundService.update(context, video = false, screen = false)

        // The call server: the API's own host, or (Tor routing) the clearnet call server the API names.
        // Calls are WebRTC through the TURN relay, never Tor, so an onion signaling address is refused.
        val url = signalingUrl(join.url)
        val signalingHost = runCatching { url.replaceFirst("ws", "http").toHttpUrl().host }.getOrNull()
        if (signalingHost == null || signalingHost.endsWith(".onion", ignoreCase = true)) {
            leaveQuietly(join.callId)
            finish(s, "Group calls aren't available on this server.")
            return
        }
        val room = try {
            // WebRTC's native library has to be loaded before the room key provider (a native object) exists.
            LiveKit.init(context.applicationContext)
            LiveKit.create(
                context.applicationContext,
                options = RoomOptions(
                    adaptiveStream = true,
                    dynacast = true,
                    e2eeOptions = E2EEOptions(keyProvider = s.keys),
                    audioTrackCaptureDefaults = LocalAudioTrackOptions(
                        noiseSuppression = true,
                        echoCancellation = echo,
                        autoGainControl = true,
                        highPassFilter = true,
                        typingNoiseDetection = false,
                    ),
                ),
                overrides = LiveKitOverrides(
                    okHttpClient = signalingClient(signalingHost),
                    audioOptions = AudioOptions(
                        // CallAudio does the routing (earpiece, speaker, focus), as in one-to-one calls.
                        audioHandler = NoAudioHandler(),
                        javaAudioDeviceModuleCustomizer = { builder ->
                            builder.setUseHardwareAcousticEchoCanceler(echo)
                            builder.setUseHardwareNoiseSuppressor(true)
                        },
                    ),
                ),
            )
        } catch (e: Exception) {
            if (!s.closed) {
                leaveQuietly(join.callId)
                finish(s, "Couldn't set up the call. Try again in a minute.")
            }
            return
        }
        s.room = room
        s.eventsJob = scope.launch { room.events.events.collect { onRoomEvent(s, it) } }
        if (s.closed) return release(s)

        try {
            // Relay only: media goes through the call server's TURN relay, so no participant
            // learns another's IP address (the same as one-to-one calls and the other apps).
            room.connect(url, join.token, ConnectOptions(rtcConfig = PeerConnection.RTCConfiguration(emptyList()).apply {
                iceTransportsType = PeerConnection.IceTransportsType.RELAY
            }))
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (BuildConfig.DEBUG) Log.w(TAG, "connect failed", e)
            if (!s.closed) {
                leaveQuietly(join.callId)
                finish(s, "Couldn't reach the call server. Try again in a minute.")
            }
            return
        }
        if (s.closed) return release(s)

        patch { it.copy(phase = GroupPhase.Active, connectedAt = System.currentTimeMillis()) }
        s.ready = true
        // The link goes out only now that the call is really on: until the starter is
        // connected there's nothing for anyone to join.
        if (s.announce) {
            s.announce = false
            postCallLink(s)
        }
        if (BuildConfig.DEBUG) probeStats(s)
        val buffered = s.buffer.filter { it.str("callId") == join.callId }
        s.buffer.clear()
        for (event in buffered) handleCallEvent(s, event)
        refreshTiles()
    }

    /**
     * Debug builds only: what actually arrives from each person, after decryption and decoding, every few
     * seconds in logcat (`adb logcat -s GroupCall`), for the end-to-end tests. Compiled out of release builds.
     */
    private fun probeStats(s: Session) {
        scope.launch {
            while (!s.closed) {
                delay(STATS_EVERY_MS)
                val room = s.room ?: break
                for (participant in room.remoteParticipants.values) {
                    for (publication in participant.trackPublications.values) {
                        val track = publication.track ?: continue
                        val report = runCatching { track.getRTCStats() }.getOrNull() ?: continue
                        for (stat in report.statsMap.values) {
                            if (stat.type != "inbound-rtp") continue
                            val m = stat.members
                            val line = "inbound ${participant.identity?.value} ${track.kind}: packets=${m["packetsReceived"]} " +
                                "decoded=${m["framesDecoded"] ?: m["totalSamplesReceived"]} " +
                                "level=${m["audioLevel"]} energy=${m["totalAudioEnergy"]} concealed=${m["concealedSamples"]} " +
                                "size=${m["frameWidth"]}x${m["frameHeight"]} lost=${m["packetsLost"]}"
                            if (BuildConfig.DEBUG) Log.d(TAG, line)
                        }
                    }
                }
            }
        }
    }

    /** wss://<the API's host><path> for a path on the site; anything else (a full URL) as it is. */
    private fun signalingUrl(url: String): String {
        if (!url.startsWith("/")) return url
        // https://host[:port] -> wss://host[:port] (http -> ws, for the debug builds' local server).
        val origin = apiBase.toHttpUrl().newBuilder().encodedPath("/").query(null).fragment(null).build().toString().removeSuffix("/")
        return "ws${origin.removePrefix("http")}$url"
    }

    /** Posts the call's link in the group chat, as an ordinary end-to-end encrypted message. */
    private fun postCallLink(s: Session) {
        val callId = s.callId ?: return
        sendChatMessage(s.conversationId, GroupCallLink.inviteText(s.kind, GroupCallLink.url(webAppBase, callId))) { sent ->
            if (!sent && !s.closed) patch { it.copy(notice = "Couldn't post the call link in the chat. People can still join from the Join button.") }
        }
    }

    // --- Room events ---

    private fun onRoomEvent(s: Session, event: RoomEvent) {
        if (s.closed || session !== s) return
        when (event) {
            is RoomEvent.Reconnecting -> patch { it.copy(phase = GroupPhase.Reconnecting) }
            is RoomEvent.Reconnected -> patch { it.copy(phase = GroupPhase.Active) }
            is RoomEvent.Disconnected -> {
                s.callId?.let { leaveQuietly(it) }
                finish(s, "Call dropped")
            }
            is RoomEvent.TrackE2EEStateEvent -> {
                if (BuildConfig.DEBUG) Log.d(TAG, "e2ee ${event.participant.identity?.value} ${event.track.kind} -> ${event.state}")
                val failures = if (event.participant is LocalParticipant) s.sendFailures else s.receiveFailures
                failures.report(event.publication.sid, event.state, SystemClock.elapsedRealtime())
                showEncryptionState(s)
                refreshTiles()
            }
            is RoomEvent.ParticipantConnected, is RoomEvent.ParticipantDisconnected,
            is RoomEvent.TrackPublished, is RoomEvent.TrackUnpublished,
            is RoomEvent.TrackSubscribed, is RoomEvent.TrackUnsubscribed,
            is RoomEvent.TrackMuted, is RoomEvent.TrackUnmuted,
            is RoomEvent.ActiveSpeakersChanged,
            -> refreshTiles()
            else -> Unit
        }
    }

    /** Rebuilds the tiles from the LiveKit room and the roster (which maps pseudonyms to people). */
    private fun refreshTiles() {
        val s = session ?: return
        val room = s.room ?: return
        val group = groupOf(s.conversationId)
        val byIdentity = s.members.associate { it.identity to it.userId }
        val tiles = mutableListOf<GroupTile>()
        val frontCamera = state.value.call?.frontCamera ?: true
        fun add(participant: Participant, isSelf: Boolean) {
            val userId = if (isSelf) identity.userId else byIdentity[participant.identity?.value]
            val member = userId?.let { id -> group?.meta?.members?.firstOrNull { it.userId == id } }
            val handle = if (isSelf) "You" else member?.handle ?: "Joining…"
            val avatar = if (isSelf) identity.avatarUrl else member?.avatarUrl
            val camera = participant.getTrackPublication(Track.Source.CAMERA)
            val screen = participant.getTrackPublication(Track.Source.SCREEN_SHARE)
            val micMuted = !participant.isMicrophoneEnabled
            val cameraTrack = if (camera != null && !camera.muted) camera.track as? io.livekit.android.room.track.VideoTrack else null
            val id = participant.identity?.value ?: if (isSelf) "self" else "?"
            tiles += GroupTile("$id:camera", userId, handle, avatar, isSelf, false, cameraTrack, micMuted, participant.isSpeaking, mirrored = isSelf && frontCamera)
            (screen?.track as? io.livekit.android.room.track.VideoTrack)?.let { track ->
                tiles += GroupTile("$id:screen", userId, handle, avatar, isSelf, true, track, micMuted, false)
            }
        }
        add(room.localParticipant, true)
        room.remoteParticipants.values.forEach { add(it, false) }
        val local = room.localParticipant
        val sentGone = s.sendFailures.keep(local.trackPublications.keys)
        val receivedGone = s.receiveFailures.keep(room.remoteParticipants.values.flatMapTo(HashSet()) { it.trackPublications.keys })
        if (sentGone || receivedGone) showEncryptionState(s)
        patch {
            it.copy(
                tiles = tiles,
                muted = !local.isMicrophoneEnabled && s.published,
                cameraOff = if (s.published) !local.isCameraEnabled else it.cameraOff,
                sharingScreen = local.isScreenShareEnabled,
            )
        }
    }

    /**
     * Tells the person when their audio or video can't be encrypted (it isn't going out) or someone's can't be
     * decrypted, and takes the message away once that's over. A failure to decrypt gets a moment to pass first.
     */
    private fun showEncryptionState(s: Session) {
        if (s.closed || session !== s) return
        val now = SystemClock.elapsedRealtime()
        val problem = when {
            s.sendFailures.failing(now) -> CANT_ENCRYPT
            s.receiveFailures.failing(now) -> CANT_DECRYPT
            else -> null
        }
        patch { view ->
            when {
                problem != null -> view.copy(notice = problem)
                view.notice == CANT_ENCRYPT || view.notice == CANT_DECRYPT -> view.copy(notice = null)
                else -> view
            }
        }
        s.e2eeCheckJob?.cancel()
        s.e2eeCheckJob = if (s.receiveFailures.undecided(now)) scope.launch {
            delay(E2EE_GRACE_MS)
            showEncryptionState(s)
        } else null
    }

    /** This phone can't keep the call encrypted: it stops here, and says why. */
    private fun endForEncryption(s: Session) {
        if (s.closed) return
        s.callId?.let { leaveQuietly(it) }
        finish(s, ENDED_NO_ENCRYPTION)
    }

    /** What's asked of the room before anything is published or unmuted. */
    private fun encryptionOn(s: Session, room: Room): Boolean = mayPublish(room.e2eeManager?.enabled, s.keys.hasKey, s.keyEpoch)

    // --- Keys ---

    private suspend fun handleCallEvent(s: Session, event: JsonObject) = callMutex.withLock {
        if (s.closed) return@withLock
        when (event.str("type")) {
            "group-roster" -> handleRoster(s, event)
            "group-key" -> handleKey(s, event)
        }
    }

    private suspend fun handleRoster(s: Session, event: JsonObject) {
        val epoch = event.long("epoch")
        if (epoch <= s.epoch) return
        s.epoch = epoch
        s.keyholder = event.str("keyholder")
        s.members = (event["members"] as? JsonArray).orEmpty().mapNotNull { m ->
            val o = m as? JsonObject ?: return@mapNotNull null
            RosterMember(o.str("userId"), o.str("identity")).takeIf { it.userId.isNotEmpty() }
        }
        val callId = s.callId ?: return
        _state.update { st -> st.copy(active = st.active + (s.conversationId to ActiveGroupCall(callId, s.kind, s.members.size))) }

        val known = s.members.map { it.userId to publicKeyOf(s, it.userId) }
        val complete = known.all { it.second != null }
        patch {
            it.copy(
                keyReady = s.keyEpoch == epoch,
                sas = if (complete) crypto.groupSasWords(callId, known.map { (id, key) -> id to key!! }) else null,
            )
        }
        refreshTiles()

        if (s.keyholder == identity.userId) {
            // Our turn: a fresh key for this epoch, sealed to everyone else.
            val key = crypto.newRoomKey()
            applyKey(s, key, epoch)
            if (s.closed) {
                key.fill(0)
                return
            }
            val sealed = s.members.filter { it.userId != identity.userId }.mapNotNull { m ->
                val publicKey = publicKeyOf(s, m.userId) ?: return@mapNotNull null
                val message = ExpectedRoomKey(callId, s.conversationId, epoch, identity.userId, m.userId)
                val box = crypto.sealRoomKey(message, key, publicKey, identity.privateKey)
                GroupKeyEntry(m.userId, SealedSignalDto(Base64Codec.encode(box.ciphertext), Base64Codec.encode(box.nonce)))
            }
            if (sealed.isNotEmpty()) {
                try {
                    apiCall(ApiClient.json) { api.sendGroupCallKeys(callId, SendGroupKeysRequest(epoch, sealed)) }
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    // 409: someone joined or left meanwhile; the next roster brings a new epoch.
                    if (!(e is ApiException && e.status == 409)) patch { it.copy(notice = "Couldn't hand out the call key. Others may not hear you.") }
                }
            }
            key.fill(0)
        } else {
            val early = s.earlyKeys.remove(epoch)
            if (early != null) handleKey(s, early)
        }
    }

    private suspend fun handleKey(s: Session, event: JsonObject) {
        val epoch = event.long("epoch")
        if (epoch > s.epoch) {
            s.earlyKeys[epoch] = event
            return
        }
        val from = event.str("from")
        if (epoch < s.epoch || from != s.keyholder) return
        val callId = s.callId ?: return
        try {
            val senderKey = publicKeyOf(s, from) ?: throw GroupKeyException("The call key came from someone this phone has no identity key for.")
            val box = event["key"] as? JsonObject ?: throw GroupKeyException("The call key couldn't be verified as coming from the keyholder's identity key.")
            val sealed = try {
                SealedSignal(Base64Codec.decode(box.str("ciphertext")), Base64Codec.decode(box.str("nonce")))
            } catch (e: IllegalArgumentException) {
                throw GroupKeyException("The call key couldn't be verified as coming from the keyholder's identity key.")
            }
            val key = crypto.openRoomKey(sealed, senderKey, identity.privateKey, ExpectedRoomKey(callId, s.conversationId, epoch, from, identity.userId))
            applyKey(s, key, epoch)
            key.fill(0)
        } catch (e: GroupKeyException) {
            // Not usable: stay silent rather than use a key that didn't verify.
            patch { it.copy(notice = e.message ?: "Couldn't verify the call key.") }
        }
    }

    private suspend fun applyKey(s: Session, key: ByteArray, epoch: Long) {
        if (!installOrSilence({ s.keys.installKey(key, epoch) }, publishedSenders(s))) return endForEncryption(s)
        s.keyEpoch = maxOf(s.keyEpoch, epoch)
        if (!rekeySenders(s)) return endForEncryption(s)
        if (s.keyEpoch == s.epoch) patch { it.copy(keyReady = true, notice = null) }
        showEncryptionState(s)
        maybePublish(s)
    }

    /**
     * Switches what this phone sends to the newest epoch's key slot. LiveKit sets a sender's key slot when
     * its encryption is attached to a track, and nothing moves it afterwards, so each published track gets
     * its encryption attached afresh, with the tracks muted throughout (see [moveToNewKey] in GroupCallGuards.kt).
     * False if a track couldn't be moved: its tracks stay muted, and the caller ends the call.
     */
    private suspend fun rekeySenders(s: Session): Boolean {
        val room = s.room ?: return false
        val slot = s.keys.sendSlot
        if (s.cryptorSlot == slot) return true
        val moved = moveToNewKey(publishedSenders(s)) { delay(ENCODER_SETTLE_MS) }
        if (!moved || s.closed) return false
        s.cryptorSlot = slot
        return true
    }

    private fun publishedSenders(s: Session): List<PublishedSender> {
        val room = s.room ?: return emptyList()
        return room.localParticipant.trackPublications.values.filterIsInstance<LocalTrackPublication>().mapNotNull { publication ->
            publication.track?.let { PublishedSender(room, publication, it) }
        }
    }

    private class PublishedSender(private val room: Room, private val publication: LocalTrackPublication, private val track: Track) : RekeyableSender {
        override var enabled: Boolean
            get() = track.enabled
            set(value) {
                track.enabled = value
            }

        override fun reattach() {
            val manager = room.e2eeManager?.takeIf { it.enabled } ?: throw IllegalStateException("The room's E2EE is off")
            manager.removePublishedTrack(track, publication, room.localParticipant, room)
            manager.addPublishedTrack(track, publication, room.localParticipant, room)
        }
    }

    /** Starts sending once this phone holds a room key: never before. */
    private suspend fun maybePublish(s: Session) {
        val room = s.room ?: return
        if (s.published || s.keyEpoch == 0L || s.closed) return
        if (!encryptionOn(s, room)) return endForEncryption(s)
        s.published = true
        val local = room.localParticipant
        try {
            val track = local.createAudioTrack("microphone")
            // Muted until its frames are being encrypted; unmuted right after.
            track.enabled = false
            if (!local.publishAudioTrack(track)) throw IllegalStateException("The microphone track wasn't published")
            s.cryptorSlot = s.keys.sendSlot
            track.enabled = true
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (BuildConfig.DEBUG) Log.w(TAG, "microphone publish failed", e)
            if (s.closed) return
            s.callId?.let { leaveQuietly(it) }
            finish(s, "Couldn't use your microphone. Allow access to join the call.")
            return
        }
        if (s.kind == "video") {
            if (!publishCamera(room)) patch { it.copy(cameraOff = true, notice = "Couldn't use your camera. Allow access to share video.") }
        }
        refreshTiles()
    }

    /** Publishes the camera, muted until its frames are being encrypted. */
    private suspend fun publishCamera(room: Room): Boolean {
        val local = room.localParticipant
        if (androidx.core.content.ContextCompat.checkSelfPermission(context, android.Manifest.permission.CAMERA) !=
            android.content.pm.PackageManager.PERMISSION_GRANTED
        ) {
            return false
        }
        var track: LocalVideoTrack? = null
        return try {
            val created = local.createVideoTrack(
                "camera",
                LocalVideoTrackOptions(position = CameraPosition.FRONT, captureParams = VideoPreset169.H540.capture),
            )
            track = created
            created.enabled = false
            created.startCapture()
            if (!local.publishVideoTrack(created)) throw IllegalStateException("The camera track wasn't published")
            created.enabled = true
            CallForegroundService.update(context, video = true, screen = state.value.call?.sharingScreen == true)
            true
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (BuildConfig.DEBUG) Log.w(TAG, "camera publish failed", e)
            track?.let { dropFailedVideo(local, it) }
            false
        }
    }

    /**
     * A track that failed partway may be published already, with no encryption attached (LiveKit adds the publication
     * before it attaches the cryptor). It can't stay: turning the camera on again would just unmute it.
     */
    private fun dropFailedVideo(local: LocalParticipant, track: LocalVideoTrack) {
        runCatching { track.stopCapture() }
        runCatching { local.unpublishTrack(track) }
        runCatching { track.stop() }
    }

    private fun publicKeyOf(s: Session, userId: String): ByteArray? {
        if (userId == identity.userId) return identity.publicKey
        val member = groupOf(s.conversationId)?.meta?.members?.firstOrNull { it.userId == userId } ?: return null
        return runCatching { Base64Codec.decode(member.publicKeyB64) }.getOrNull()?.takeIf { it.isNotEmpty() }
    }

    // --- In the call ---

    fun toggleMute() {
        val s = session ?: return
        val local = s.room?.localParticipant ?: return
        if (!s.published) return
        val room = s.room ?: return
        scope.launch {
            // Never in the middle of a key change, which holds the tracks muted.
            callMutex.withLock {
                val enable = !local.isMicrophoneEnabled
                if (enable && !encryptionOn(s, room)) return@withLock patch { it.copy(notice = ENCRYPTION_OFF) }
                runCatching { local.setMicrophoneEnabled(enable) }
                patch { it.copy(muted = !enable) }
            }
            refreshTiles()
        }
    }

    /** Camera on/off (video calls only, and only once this phone holds a key). */
    fun toggleCamera() {
        val s = session ?: return
        val room = s.room ?: return
        if (s.kind != "video" || s.keyEpoch == 0L || !s.published) return
        val local = room.localParticipant
        scope.launch {
            callMutex.withLock {
                try {
                    if (local.isCameraEnabled) {
                        local.setCameraEnabled(false)
                        patch { it.copy(cameraOff = true) }
                        CallForegroundService.update(context, video = false, screen = state.value.call?.sharingScreen == true)
                    } else if (!encryptionOn(s, room)) {
                        patch { it.copy(notice = ENCRYPTION_OFF) }
                    } else if (local.getTrackPublication(Track.Source.CAMERA) != null) {
                        CallForegroundService.update(context, video = true, screen = state.value.call?.sharingScreen == true)
                        local.setCameraEnabled(true)
                        patch { it.copy(cameraOff = false, notice = null) }
                    } else if (publishCamera(room)) {
                        patch { it.copy(cameraOff = false, notice = null) }
                    } else {
                        patch { it.copy(notice = "Couldn't turn on your camera. Allow access to use it.") }
                    }
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    patch { it.copy(notice = "Couldn't use your camera.") }
                }
            }
            refreshTiles()
        }
    }

    fun flipCamera() {
        val track = session?.room?.localParticipant?.getTrackPublication(Track.Source.CAMERA)?.track as? LocalVideoTrack ?: return
        track.switchCamera()
        patch { it.copy(frontCamera = !it.frontCamera) }
        refreshTiles()
    }

    fun toggleSpeaker() {
        val on = !(state.value.call?.speakerOn ?: return)
        audio.setSpeaker(on)
        patch { it.copy(speakerOn = on) }
    }

    val canShareScreen: Boolean get() = tier() == "premium"

    /** Starts sharing the screen with the system's capture permission [projectionData]. Premium only (the server's token checks too). */
    fun startScreenShare(projectionData: Intent) {
        val s = session ?: return
        val room = s.room ?: return
        if (!canShareScreen) return patch { it.copy(notice = "Screen sharing is part of Premium.") }
        if (s.keyEpoch == 0L || !s.published) return
        val local = room.localParticipant
        scope.launch {
            callMutex.withLock {
                if (!encryptionOn(s, room)) return@withLock patch { it.copy(notice = ENCRYPTION_OFF) }
                // Android 14+: the foreground service must hold the media-projection type before capture starts.
                CallForegroundService.update(context, video = state.value.call?.cameraOff == false, screen = true)
                delay(SERVICE_SETTLE_MS)
                var started: io.livekit.android.room.track.LocalScreencastVideoTrack? = null
                try {
                    val track = local.createScreencastTrack("screen", projectionData) {
                        // The system (or the user, from its own notification) ended the capture.
                        scope.launch { stopScreenShare() }
                    }
                    started = track
                    track.enabled = false
                    track.startCapture()
                    if (!local.publishVideoTrack(track)) throw IllegalStateException("The screen track wasn't published")
                    track.enabled = true
                    patch { it.copy(sharingScreen = true, notice = null) }
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    if (BuildConfig.DEBUG) Log.w(TAG, "screen share failed", e)
                    started?.let { dropFailedVideo(local, it) }
                    CallForegroundService.update(context, video = state.value.call?.cameraOff == false, screen = false)
                    patch { it.copy(sharingScreen = false, notice = "Couldn't share your screen.") }
                }
            }
            refreshTiles()
        }
    }

    fun stopScreenShare() {
        val s = session ?: return
        val local = s.room?.localParticipant ?: return
        val track = local.getTrackPublication(Track.Source.SCREEN_SHARE)?.track
        if (track != null) local.unpublishTrack(track)
        CallForegroundService.update(context, video = state.value.call?.cameraOff == false, screen = false)
        patch { it.copy(sharingScreen = false) }
        refreshTiles()
    }

    // --- Video surfaces ---

    /** Shows [track] on [renderer] (a fresh one is set up on this call's EGL context). */
    fun attach(renderer: TextureViewRenderer, track: io.livekit.android.room.track.VideoTrack) {
        val room = session?.room ?: return
        room.initVideoRenderer(renderer)
        track.addRenderer(renderer)
    }

    fun detach(renderer: TextureViewRenderer, track: io.livekit.android.room.track.VideoTrack) {
        runCatching { track.removeRenderer(renderer) }
        runCatching { renderer.release() }
    }

    // --- Live events ---

    private suspend fun handleEvent(event: JsonObject) {
        val type = event.str("type")
        if (!type.startsWith("group-")) return
        val callId = event.str("callId")
        val conversationId = event.str("conversationId")

        if (type == "group-removed") return onRemovedFromCall(callId)

        if (type == "group-incoming") {
            _state.update { st -> st.copy(active = st.active + (conversationId to ActiveGroupCall(callId, kindOf(event), 1))) }
            val group = groupOf(conversationId)
            val hostId = event.str("hostId")
            if (group == null || session != null || oneToOneBusy() || hostId == identity.userId) return
            endedJob?.cancel()
            val s = Session(callId, group.id, kindOf(event))
            s.ringing = true
            val host = group.meta.members.firstOrNull { it.userId == hostId }
            begin(s, blankView(group, s.kind, GroupPhase.Incoming, callId, host?.handle))
            // Muted groups' calls show, but don't ring; with the ringtone off, no call rings.
            if (ringtone() && group.id !in live.state.value.muted) audio.ring()
            return
        }

        if (type == "group-ended") {
            _state.update { st ->
                if (st.active[conversationId]?.callId == callId) st.copy(active = st.active - conversationId) else st
            }
        }

        val s = session ?: return
        // Joining: hold on to this call's events until the room is connected.
        if (!s.ready) {
            if (type == "group-roster" || type == "group-key") s.buffer += event
            else if (s.callId == callId) {
                if (s.ringing) audio.stopTones()
                finish(s, if (type == "group-ended") "Call ended" else null)
            }
            return
        }
        if (s.callId != callId) return
        if (type == "group-ended") finish(s, "Call ended") else handleCallEvent(s, event)
    }

    /**
     * The server took this account out of the group, so it's out of the call, and the others have moved to a key
     * it won't get: leave at once, whatever stage the call is at. There's nothing to tell the server.
     */
    private fun onRemovedFromCall(callId: String) {
        if (callId.isEmpty()) return
        removedFrom += callId
        _state.update { st -> st.copy(active = st.active.withoutCall(callId)) }
        val s = session ?: return
        if (s.callId != callId) return
        if (s.ringing) audio.stopTones()
        finish(s, REMOVED_FROM_CALL)
    }

    // --- Calls going on in your groups (for the Join banner) ---

    /** Asks which of your groups have a call going on. */
    fun refreshActive() {
        if (!_state.value.available) return
        scope.launch {
            try {
                val calls = apiCall(ApiClient.json) { api.activeGroupCalls() }.calls
                _state.update { st -> st.copy(active = calls.associate { it.conversationId to ActiveGroupCall(it.callId, it.kind, it.participants) }) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                if (e is ApiException && e.status == 401) onSessionExpired()
            }
        }
    }

    // --- Ending ---

    /** Tears the session down and leaves a closing banner (or nothing). */
    private fun finish(s: Session, message: String?) {
        if (s.closed) return
        s.closed = true
        if (session === s) session = null
        s.eventsJob?.cancel()
        s.e2eeCheckJob?.cancel()
        release(s)
        audio.stop()
        if (session == null) {
            CallForegroundService.stop(context)
            live.release(HOLD_TAG)
        }
        endedJob?.cancel()
        if (message != null) {
            _state.update { st ->
                st.copy(call = st.call?.copy(phase = GroupPhase.Ended, endedMessage = message, tiles = emptyList(), sharingScreen = false))
            }
            endedJob = scope.launch {
                delay(ENDED_BANNER_MS)
                _state.update { it.copy(call = null) }
            }
        } else {
            _state.update { it.copy(call = null) }
        }
    }

    private fun release(s: Session) {
        val room = s.room ?: return
        s.room = null
        runCatching { room.disconnect() }
        runCatching { room.release() }
        if (s.keysLazy.isInitialized()) runCatching { s.keys.dispose() }
    }

    // --- Helpers ---

    /**
     * Not communities: a call's room key is sealed to each member's identity key from the group's member
     * list, and on this phone a community's list holds only the people seen posting (so their calls don't
     * ring here either).
     */
    private fun groupOf(conversationId: String): Conversation? =
        conversations().firstOrNull { it.isGroup && it.meta.community == null && it.id == conversationId }

    private fun blankView(group: Conversation, kind: String, phase: GroupPhase, callId: String?, hostHandle: String?) = GroupCallView(
        callId = callId,
        conversationId = group.id,
        groupLabel = group.handle,
        kind = kind,
        phase = phase,
        hostHandle = hostHandle,
        cameraOff = true,
    )

    private fun kindOf(event: JsonObject) = if (event.str("kind") == "video") "video" else "voice"

    private fun patch(change: (GroupCallView) -> GroupCallView) = _state.update { st -> st.copy(call = st.call?.let(change)) }

    private fun describe(e: Exception, fallback: String): String {
        if (e is ApiException) {
            if (e.status == 401) onSessionExpired()
            if (e.code == "premium_required") return PREMIUM_ONLY
            if (e.code == "group_calls_disabled") return NOT_AVAILABLE
            return e.message ?: fallback
        }
        return fallback
    }

    private fun JsonObject.str(key: String): String = (this[key] as? JsonPrimitive)?.takeIf { it.isString }?.content.orEmpty()

    private fun JsonObject.long(key: String): Long = (this[key] as? JsonPrimitive)?.content?.toLongOrNull() ?: 0L

    companion object {
        private const val TAG = "GroupCall"
        private const val HOLD_TAG = "groupcall"
        const val ENDED_BANNER_MS = 4000L
        /** Lets the foreground service pick up its new media-projection type before capture starts. */
        private const val SERVICE_SETTLE_MS = 400L
        private const val STATS_EVERY_MS = 5000L
        /** How long the tracks stay muted before their encryption changes, for frames already being encoded. */
        private const val ENCODER_SETTLE_MS = 120L
        /** How long a track may fail to decrypt before it's mentioned: longer than a key change takes. */
        private const val E2EE_GRACE_MS = 4000L
        const val PREMIUM_ONLY = "Only Premium members can start group calls"
        const val ALREADY_IN_CALL = "You're already in a call. Leave it first."
        const val NOT_AVAILABLE = "Group calls aren't available on this server."
        const val COMMUNITY_CALLS = "Calls in communities aren't in the Android app yet. Join them on the web or desktop."
        const val REMOVED_FROM_CALL = "You were removed from this group, so you've left its call."
        const val ENDED_NO_ENCRYPTION = "Call ended: this phone couldn't keep it end-to-end encrypted."
        const val ENCRYPTION_OFF = "End-to-end encryption isn't on for this call, so nothing was turned on."
        const val CANT_ENCRYPT = "Your audio or video couldn't be encrypted, so it isn't being sent. Leave the call and join again."
        const val CANT_DECRYPT = "Someone's audio or video can't be decrypted. Compare the safety words, or leave and join again."
    }
}

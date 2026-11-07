package com.redvaultsystems.redvaultmessage.ui.live

import com.redvaultsystems.redvaultmessage.data.ContactPrefsStore
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.LiveApi
import com.redvaultsystems.redvaultmessage.data.api.TypingRequest
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive

data class LiveState(
    /** Whether this server has calls turned on (from the live channel). */
    val callsEnabled: Boolean = false,
    val groupCallsEnabled: Boolean = false,
    /** Contacts online right now (empty unless you share your own status). */
    val online: Set<String> = emptySet(),
    /** chat id -> user id -> shown until (ms). Entries past their time are pruned every second. */
    val typing: Map<String, Map<String, Long>> = emptyMap(),
    val typingIndicators: Boolean = false,
    val favorites: Set<String> = emptySet(),
    val muted: Set<String> = emptySet(),
) {
    fun typingIn(chatId: String): List<String> = typing[chatId]?.keys?.toList().orEmpty()
}

/**
 * The live channel (GET /api/live/events, a long poll) and what rides on
 * it: call signaling (re-emitted on [events] for calls), typing indicators
 * and contacts' online status. A port of the desktop's LiveSession and the
 * web's LiveContext. Being on the channel is what makes you "online" to
 * contacts who can see it, so it runs only while the app is in use.
 *
 * Typing is opt-in and reciprocal: sent, and others' shown, only while
 * typing indicators are on here. Online status is too, on the server: you
 * see who's online only while you share your own.
 */
class LiveSession(
    private val api: LiveApi,
    private val identity: SelfIdentity,
    private val prefs: ContactPrefsStore,
    private val onSessionExpired: () -> Unit,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow(
        LiveState(
            typingIndicators = prefs.typingIndicators(identity.userId),
            favorites = prefs.favorites(identity.userId),
            muted = prefs.muted(identity.userId),
        ),
    )
    val state: StateFlow<LiveState> = _state.asStateFlow()

    private val _events = MutableSharedFlow<JsonObject>(extraBufferCapacity = 64)
    /** Everything but typing: "incoming", "answered", "signal", "screen", "ended", ... */
    val events: SharedFlow<JsonObject> = _events.asSharedFlow()

    @Volatile private var presenceEnabled = false
    private val lastTypingSent = HashMap<String, Long>()
    private val holders = HashSet<String>()
    private var pollJob: Job? = null

    /**
     * Keeps the long poll running while anyone holds it: the app's screen
     * while it's on screen, and a call for as long as it lasts (the server
     * ends a call whose apps stop listening, so a call in the background
     * must keep it going).
     */
    fun hold(tag: String) {
        holders += tag
        if (pollJob?.isActive != true) pollJob = scope.launch { run() }
    }

    fun release(tag: String) {
        holders -= tag
        if (holders.isEmpty()) {
            pollJob?.cancel()
            pollJob = null
        }
    }

    /** Holds the channel until the calling coroutine is cancelled (e.g. inside repeatOnLifecycle). */
    suspend fun holdWhileActive(tag: String) {
        hold(tag)
        try {
            awaitCancellation()
        } finally {
            release(tag)
        }
    }

    /** The long poll, until cancelled. */
    private suspend fun run() {
        var cursor: Long? = null
        var failures = 0
        while (true) {
            try {
                val result = apiCall(ApiClient.json) { api.events(cursor) }
                cursor = result.cursor
                failures = 0
                _state.update { it.copy(callsEnabled = result.callsEnabled, groupCallsEnabled = result.groupCallsEnabled) }
                result.events.forEach(::deliver)
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                when {
                    e.status == 401 -> return onSessionExpired()
                    // A server without the live channel, or with calls off entirely: stop quietly.
                    e.status == 404 || e.code == "calls_disabled" -> return _state.update { it.copy(callsEnabled = false) }
                    else -> failures++
                }
            } catch (e: Exception) {
                failures++
            }
            if (failures > 0) delay(minOf(30_000L, 2_000L * failures))
        }
    }

    /** Online status every 20 s while shared, and typing dots fading out, until cancelled. */
    suspend fun tick() {
        var elapsed = PRESENCE_EVERY_MS
        while (true) {
            if (presenceEnabled && elapsed >= PRESENCE_EVERY_MS) {
                elapsed = 0
                refreshPresence()
            }
            pruneTyping()
            delay(1000)
            elapsed += 1000
        }
    }

    private fun deliver(event: JsonObject) {
        val type = event.str("type")
        if (type == "typing") {
            if (!_state.value.typingIndicators) return
            val chat = event.str("conversationId")
            val user = event.str("userId")
            if (chat.isEmpty() || user.isEmpty()) return
            val until = System.currentTimeMillis() + TYPING_SHOWN_MS
            _state.update { s -> s.copy(typing = s.typing + (chat to (s.typing[chat].orEmpty() + (user to until)))) }
            return
        }
        _events.tryEmit(event)
    }

    private fun pruneTyping() {
        val now = System.currentTimeMillis()
        val current = _state.value.typing
        if (current.isEmpty()) return
        val next = current.mapValues { (_, users) -> users.filterValues { it > now } }.filterValues { it.isNotEmpty() }
        if (next != current) _state.update { it.copy(typing = next) }
    }

    /** Call on each edit of the message box: throttled, and nothing is sent unless typing indicators are on. */
    fun notifyTyping(chatId: String) {
        if (!_state.value.typingIndicators || chatId.isEmpty()) return
        val now = System.currentTimeMillis()
        if (now - (lastTypingSent[chatId] ?: 0L) < TYPING_SEND_EVERY_MS) return
        lastTypingSent[chatId] = now
        scope.launch { runCatching { apiCall(ApiClient.json) { api.typing(TypingRequest(chatId)) } } }
    }

    fun setTypingIndicators(on: Boolean) {
        prefs.setTypingIndicators(identity.userId, on)
        _state.update { it.copy(typingIndicators = on, typing = if (on) it.typing else emptyMap()) }
    }

    /** Whether this account shares its online status (the server's setting, from the profile). */
    fun setPresenceEnabled(on: Boolean) {
        if (presenceEnabled == on) return
        presenceEnabled = on
        if (on) scope.launch { refreshPresence() } else _state.update { it.copy(online = emptySet()) }
    }

    private suspend fun refreshPresence() {
        if (!presenceEnabled) return
        try {
            val online = apiCall(ApiClient.json) { api.presence() }.online.toSet()
            if (presenceEnabled) _state.update { it.copy(online = online) }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // Tried again on the next turn.
        }
    }

    fun toggleFavorite(userId: String) {
        val next = _state.value.favorites.let { if (userId in it) it - userId else it + userId }
        prefs.setFavorites(identity.userId, next)
        _state.update { it.copy(favorites = next) }
    }

    /** Muted: no notifications from them, and their calls don't ring. */
    fun toggleMuted(userId: String) {
        val next = _state.value.muted.let { if (userId in it) it - userId else it + userId }
        prefs.setMuted(identity.userId, next)
        _state.update { it.copy(muted = next) }
    }

    fun close() {
        scope.cancel()
        _state.update { it.copy(online = emptySet(), typing = emptyMap()) }
    }

    private fun JsonObject.str(key: String): String = (this[key] as? JsonPrimitive)?.takeIf { it.isString }?.content.orEmpty()

    companion object {
        const val TYPING_SHOWN_MS = 6_000L
        const val TYPING_SEND_EVERY_MS = 3_000L
        const val PRESENCE_EVERY_MS = 20_000L
    }
}

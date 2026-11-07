package com.redvaultsystems.redvaultmessage.ui.inbox

import com.redvaultsystems.redvaultmessage.data.api.AnnouncementDto
import com.redvaultsystems.redvaultmessage.data.api.AnnouncementReplyDto
import com.redvaultsystems.redvaultmessage.data.api.AnnouncementReplyRequest
import com.redvaultsystems.redvaultmessage.data.api.AnnouncementVoteRequest
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class InboxState(
    /** Newest first. */
    val items: List<AnnouncementDto> = emptyList(),
    val loaded: Boolean = false,
    /** The team's replies you can see, by announcement id; fetched when an item that allows replies is shown. */
    val replies: Map<String, List<AnnouncementReplyDto>> = emptyMap(),
    val busy: Boolean = false,
    val error: String? = null,
) {
    val unread: Int get() = items.count { !it.read }
}

/**
 * The Inbox: messages and polls the Redvault team sends from the web admin panel (/api/announcements), the
 * counterpart of the web's screens/Announcements.tsx and the desktop's InboxView. They are NOT end-to-end
 * encrypted (the server counts votes) and the page says so. Checked every minute while the app is on screen,
 * for the badge in Settings; opening the page marks everything read.
 */
class InboxSession(
    private val api: ApiService,
    private val onSessionExpired: () -> Unit,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow(InboxState())
    val state: StateFlow<InboxState> = _state.asStateFlow()

    /** Refreshes until cancelled. Run it while the app is on screen. */
    suspend fun poll() {
        while (true) {
            refreshNow()
            delay(POLL_MS)
        }
    }

    fun refresh() {
        scope.launch { refreshNow() }
    }

    suspend fun refreshNow() {
        try {
            val items = apiCall(ApiClient.json) { api.listAnnouncements() }
            _state.update { it.copy(items = items.sortedByDescending { item -> item.createdAt }, loaded = true) }
        } catch (e: CancellationException) {
            throw e
        } catch (e: ApiException) {
            if (e.status == 401) onSessionExpired()
        } catch (e: Exception) {
            // Shown on the next try; the badge keeps its last count.
        }
    }

    /** The page is open: everything in it counts as read (like the web app). */
    fun markAllRead() {
        val unread = _state.value.items.filter { !it.read }
        if (unread.isEmpty()) return
        _state.update { state -> state.copy(items = state.items.map { it.copy(read = true) }) }
        scope.launch {
            for (item in unread) {
                try {
                    apiCall(ApiClient.json) { api.markAnnouncementRead(item.id) }
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    // Counted unread again on the next refresh.
                }
            }
        }
    }

    /** Fetches an item's replies (only items that allow them have any). */
    fun loadReplies(id: String) {
        scope.launch {
            try {
                val detail = apiCall(ApiClient.json) { api.getAnnouncement(id) }
                _state.update { it.copy(replies = it.replies + (id to detail.replies)) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                if (e.status == 401) onSessionExpired()
            } catch (e: Exception) {
                // The item still shows; replies appear on the next open.
            }
        }
    }

    /** One vote per poll, final. */
    fun vote(id: String, optionId: String) {
        run("Couldn't send your vote. Try again.") {
            apiCall(ApiClient.json) { api.voteAnnouncement(id, AnnouncementVoteRequest(optionId)) }
            _state.update { state ->
                state.copy(items = state.items.map { if (it.id == id) it.copy(myOptionId = optionId) else it })
            }
        }
    }

    /** Goes to the team only; nobody else sees it. */
    fun reply(id: String, body: String, onSent: () -> Unit) {
        val text = body.trim()
        if (text.isEmpty()) return
        run("Couldn't send your reply. Try again.") {
            val sent = apiCall(ApiClient.json) { api.replyToAnnouncement(id, AnnouncementReplyRequest(text)) }
            _state.update { it.copy(replies = it.replies + (id to (it.replies[id].orEmpty() + sent))) }
            onSent()
        }
    }

    fun clearError() = _state.update { it.copy(error = null) }

    private fun run(failure: String, work: suspend () -> Unit) {
        _state.update { it.copy(busy = true, error = null) }
        scope.launch {
            try {
                work()
                _state.update { it.copy(busy = false) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                _state.update { it.copy(busy = false) }
                if (e.status == 401) onSessionExpired() else _state.update { it.copy(error = e.message?.takeIf { m -> m.isNotBlank() } ?: failure) }
            } catch (e: Exception) {
                _state.update { it.copy(busy = false, error = failure) }
            }
        }
    }

    fun close() {
        scope.cancel()
    }

    private companion object {
        const val POLL_MS = 60_000L
    }
}

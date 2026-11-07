package com.redvaultsystems.redvaultmessage.ui.stories

import android.os.SystemClock
import com.redvaultsystems.redvaultmessage.data.AuthorStories
import com.redvaultsystems.redvaultmessage.data.HighlightView
import com.redvaultsystems.redvaultmessage.data.Inbox
import com.redvaultsystems.redvaultmessage.data.LoadedStoryPrefs
import com.redvaultsystems.redvaultmessage.data.MyAnswer
import com.redvaultsystems.redvaultmessage.data.PostStage
import com.redvaultsystems.redvaultmessage.data.StoriesRepository
import com.redvaultsystems.redvaultmessage.data.StoryInteractions
import com.redvaultsystems.redvaultmessage.data.answerableStickers
import com.redvaultsystems.redvaultmessage.data.isAnonymousSticker
import com.redvaultsystems.redvaultmessage.data.Story
import com.redvaultsystems.redvaultmessage.data.DraftContent
import com.redvaultsystems.redvaultmessage.data.DraftSummary
import com.redvaultsystems.redvaultmessage.data.StoryDraft
import com.redvaultsystems.redvaultmessage.data.StoryDraftStore
import com.redvaultsystems.redvaultmessage.data.StoryFriend
import com.redvaultsystems.redvaultmessage.data.StoryInputException
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.StoryAvailability
import com.redvaultsystems.redvaultmessage.data.model.AnswerableSticker
import com.redvaultsystems.redvaultmessage.data.model.PollSticker
import com.redvaultsystems.redvaultmessage.data.model.QuestionSticker
import com.redvaultsystems.redvaultmessage.data.model.QuizSticker
import com.redvaultsystems.redvaultmessage.data.model.ResultsVisibility
import com.redvaultsystems.redvaultmessage.data.model.SliderSticker
import com.redvaultsystems.redvaultmessage.data.model.StickerTotals
import com.redvaultsystems.redvaultmessage.data.model.StoryAnswer
import com.redvaultsystems.redvaultmessage.data.model.StoryPrefs
import com.redvaultsystems.redvaultmessage.data.model.StoryReplyRef
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.File

/** What one story's stickers show beyond the story itself. */
class StoryInteractionState(
    /** What you already answered. */
    val mine: Map<String, MyAnswer> = emptyMap(),
    /** The totals the author published (or, on your own story, counted from your inbox). */
    val totals: Map<String, StickerTotals> = emptyMap(),
    /** Answers being sent right now. */
    val busy: Set<String> = emptySet(),
    /** Your own story: the answers sealed to you, opened. */
    val inbox: Inbox? = null,
) {
    fun copy(
        mine: Map<String, MyAnswer> = this.mine,
        totals: Map<String, StickerTotals> = this.totals,
        busy: Set<String> = this.busy,
        inbox: Inbox? = this.inbox,
    ) = StoryInteractionState(mine, totals, busy, inbox)
}

/** Whether Stories show at all. Only [On] puts anything on screen or asks the server for anything but its settings. */
enum class StoriesGate { Unknown, Off, On }

data class StoriesState(
    val gate: StoriesGate = StoriesGate.Unknown,
    /** Photos and videos can be posted (the server says it can keep them). */
    val media: Boolean = false,
    /** Polls, quizzes, questions and sliders take answers (the server says so). */
    val interactive: Boolean = false,
    /** How long a story may last, in seconds, as the server offers. */
    val ttlSeconds: List<Int> = emptyList(),
    val loaded: Boolean = false,
    /** Your own live stories, oldest first. */
    val own: List<Story> = emptyList(),
    /** The tray: friends with something to see, those with something new first, muted people left out. */
    val authors: List<AuthorStories> = emptyList(),
    val friends: List<StoryFriend> = emptyList(),
    val prefs: StoryPrefs = StoryPrefs.DEFAULT,
    /** Stories that leave your story are kept for you (the archive). */
    val archiveEnabled: Boolean = false,
    /** False when the settings can't be opened on this phone: then nothing is posted and views aren't announced. */
    val prefsReadable: Boolean = true,
) {
    val on: Boolean get() = gate == StoriesGate.On
}

/**
 * Stories for the signed-in account (the Android side of ui/src/state/StoriesContext.tsx). The first thing it does, and
 * the only thing while the server says Stories are off, is read `GET /api/stories/config`; with `enabled: false` it
 * holds nothing, shows nothing and asks for nothing more. It re-reads that every few minutes, so a server switched off
 * later empties it too. Keys and decrypted stories live in memory only and are wiped by [close].
 */
class StoriesSession(
    private val identity: SelfIdentity,
    private val repository: StoriesRepository,
    private val interactions: StoryInteractions,
    /** Unfinished stories, kept on this phone only. */
    private val drafts: StoryDraftStore,
    /** Sends a chat message to a friend about one of their stories (a reply or a reaction); the callback says whether it went. */
    private val sendReply: (authorId: String, text: String, ref: StoryReplyRef, onResult: (Boolean) -> Unit) -> Unit,
    /** user id to the base64 public key their chat is sealed with, for the "same key as the chat" check. */
    private val chatKeys: () -> Map<String, String>,
    private val onSessionExpired: () -> Unit,
) {
    private val _state = MutableStateFlow(StoriesState())
    val state: StateFlow<StoriesState> = _state.asStateFlow()

    // One refresh at a time, and none while a post is changing what the feed holds.
    private val lock = Mutex()
    private var lastConfigAt = 0L
    private var lastPrefsAt = 0L
    private var prefsRevision = 0
    private var lastGapFillAt = 0L
    private var allAuthors: List<AuthorStories> = emptyList()

    /** Refreshes while the app is on screen, until cancelled: the feed every minute, with the settings and config checked now and then. */
    suspend fun poll() {
        while (true) {
            quietly { refresh() }
            delay(REFRESH_MS)
        }
    }

    /** One refresh: the config first and alone, then (only if on) the settings, the friends and the feed. */
    suspend fun refresh() = lock.withLock { refreshLocked() }

    private suspend fun refreshLocked() {
        val now = SystemClock.elapsedRealtime()
        val gate = _state.value.gate
        val configEvery = when (gate) {
            StoriesGate.Unknown -> CONFIG_RETRY_MS
            StoriesGate.Off -> CONFIG_OFF_MS
            StoriesGate.On -> CONFIG_ON_MS
        }
        if (gate == StoriesGate.Unknown || now - lastConfigAt >= configEvery) {
            val config = try {
                repository.availability()
            } catch (e: ApiException) {
                // Can't tell. Nothing is shown (or kept) that wasn't already allowed, and nothing else is asked.
                lastConfigAt = now
                if (e.status == 401) onSessionExpired()
                return
            }
            lastConfigAt = now
            when (config) {
                StoryAvailability.Off -> {
                    switchOff()
                    return
                }
                is StoryAvailability.On -> {
                    if (gate != StoriesGate.On) {
                        lastPrefsAt = 0
                        _state.update { it.copy(gate = StoriesGate.On, media = config.media, interactive = config.interactive, ttlSeconds = config.ttlSeconds) }
                    } else {
                        _state.update { it.copy(media = config.media, interactive = config.interactive, ttlSeconds = config.ttlSeconds) }
                    }
                }
            }
        }
        if (!_state.value.on) return
        try {
            if (lastPrefsAt == 0L || now - lastPrefsAt >= PREFS_MS) {
                val loaded = repository.loadPrefs(identity)
                lastPrefsAt = now
                prefsRevision = loaded.revision
                _state.update { it.copy(prefs = loaded.prefs, prefsReadable = loaded.readable, archiveEnabled = loaded.archiveEnabled) }
            }
            val friends = repository.loadFriends(chatKeys())
            val feed = repository.loadFeed(identity, friends)
            allAuthors = feed.authors
            _state.update {
                // Another refresh may have switched Stories off while this one waited on the network.
                if (!it.on) it else it.copy(loaded = true, own = feed.own, authors = tray(feed.authors, it.prefs), friends = friends.values.toList())
            }
            syncOwnTotals(feed.own, friends, now)
            if (_state.value.prefsReadable && now - lastGapFillAt >= GAP_FILL_MS) {
                lastGapFillAt = now
                try {
                    repository.fillKeyGaps(identity, feed.own.associate { it.id to it.key }, friends, _state.value.prefs)
                } catch (e: ApiException) {
                    lastGapFillAt = 0
                    throw e
                }
            }
        } catch (e: ApiException) {
            if (e.status == 401) onSessionExpired()
            // The route answers 404 stories_disabled once the operator switches Stories off.
            if (e.code == "stories_disabled") switchOff()
            throw e
        }
    }

    private fun tray(authors: List<AuthorStories>, prefs: StoryPrefs): List<AuthorStories> = authors.filter { it.author.userId !in prefs.muted }

    private fun switchOff() {
        _state.value.own.forEach { it.wipe() }
        allAuthors.forEach { a -> a.stories.forEach { it.wipe() } }
        allAuthors = emptyList()
        repository.clearCache()
        interactions.forgetPublished()
        _interactions.value = emptyMap()
        lastTotalsAt.clear()
        pendingList = null
        composer = null
        _state.value = StoriesState(gate = StoriesGate.Off)
    }

    // --- What the screens do -----------------------------------------------------------------------------------

    /** The ring goes grey; the author learns you looked only if you share views (and the settings could be read). */
    suspend fun markSeen(story: Story) {
        if (story.authorId == identity.userId || story.seen) return
        val prefs = _state.value
        _state.update { s ->
            s.copy(authors = s.authors.map { a -> if (a.author.userId != story.authorId) a else AuthorStories(a.author, a.stories.map { if (it.id == story.id) story.withSeen() else it }) })
        }
        repository.markSeen(identity, story, announce = prefs.prefs.shareViews && prefs.prefsReadable)
    }

    /** Posts a story. Throws [StoryInputException] (safe to show) when something needs fixing first. */
    suspend fun publish(draft: StoryDraft, refreshAfter: Boolean = true, onStage: (PostStage) -> Unit) {
        val state = _state.value
        if (!state.on) throw StoryInputException("Stories aren't available right now.")
        if (draft.media != null && !state.media) throw StoryInputException("This server can't keep photos or videos for stories.")
        if (!state.prefsReadable) throw StoryInputException("Your story settings can't be opened on this phone, so nothing is posted. Open Stories on the web or desktop app, or try again later.")
        call {
            val friends = repository.loadFriends(chatKeys())
            repository.publish(identity, draft, friends.values, state.prefs, onStage)
        }
        if (refreshAfter) quietly { refresh() }
    }

    // --- Drafts and the composer's unfinished work ----------------------------------------------------------------

    suspend fun listDrafts(): List<DraftSummary> = withContext(Dispatchers.IO) { drafts.list(identity) }

    suspend fun saveDraft(content: DraftContent, id: String?): String = withContext(Dispatchers.IO) { drafts.save(identity, content, id) }

    suspend fun loadDraft(id: String): DraftContent? = withContext(Dispatchers.IO) { drafts.load(identity, id) }

    suspend fun deleteDraft(id: String) = withContext(Dispatchers.IO) { drafts.delete(identity, id) }

    // The composer's slides, held here so turning the phone or leaving the screen for a moment doesn't lose them.
    private var composer: ComposerModel? = null

    internal fun composerModel(): ComposerModel = composer ?: ComposerModel(_state.value.prefs.allowReplies).also { composer = it }

    internal fun discardComposer() {
        composer = null
    }

    suspend fun remove(storyId: String) {
        call { repository.delete(storyId) }
        _state.update { s -> s.copy(own = s.own.filter { it.id != storyId }) }
        quietly { refresh() }
    }

    suspend fun photo(story: Story): ByteArray = call { repository.photo(story) }

    suspend fun video(story: Story): File = call { repository.video(story) }

    // --- Stickers ---------------------------------------------------------------------------------------------

    private val _interactions = MutableStateFlow<Map<String, StoryInteractionState>>(emptyMap())

    /** Answers, totals and (for your own stories) the opened inbox, by story id. */
    val interactionState: StateFlow<Map<String, StoryInteractionState>> = _interactions.asStateFlow()

    private val lastTotalsAt = HashMap<String, Long>()

    private fun updateInteraction(storyId: String, change: (StoryInteractionState) -> StoryInteractionState) =
        _interactions.update { it + (storyId to change(it[storyId] ?: StoryInteractionState())) }

    /**
     * Loads what a story's stickers show: for someone else's, what you already answered and the totals the author published; for
     * your own, your opened inbox (and the totals are published from it). A failure leaves the stickers showing, just not answerable yet.
     */
    suspend fun openInteractions(story: Story) {
        if (story.answerableStickers().isEmpty() || !_state.value.on) return
        try {
            if (story.authorId == identity.userId) {
                val friends = _state.value.friends.associateBy { it.userId }
                val inbox = call { interactions.inbox(identity, story, friends) }
                runCatching { interactions.publishTotals(story, inbox) }
                updateInteraction(story.id) { it.copy(inbox = inbox, totals = totalsOf(story, inbox)) }
            } else {
                val author = _state.value.friends.firstOrNull { it.userId == story.authorId } ?: return
                val mine = call { interactions.mine(identity, story, author.publicKey) }
                val totals = call { interactions.totals(story) }
                updateInteraction(story.id) { it.copy(mine = mine, totals = totals) }
            }
        } catch (e: CancellationException) {
            throw e
        } catch (_: Exception) {
        }
    }

    /** Sends an answer, sealed to the story's author on this phone. Throws with a message that's safe to show. */
    suspend fun answer(story: Story, sticker: AnswerableSticker, answer: StoryAnswer) {
        val state = _state.value
        if (!state.on || !state.interactive) throw StoryInputException("Answers are switched off.")
        val author = state.friends.firstOrNull { it.userId == story.authorId } ?: throw StoryInputException("You can only answer your friends' stories.")
        updateInteraction(story.id) { it.copy(busy = it.busy + sticker.id) }
        try {
            try {
                call { interactions.submit(identity, story, author.publicKey, sticker, answer) }
            } catch (e: ApiException) {
                if (e.code == "response_final") throw StoryInputException("You already answered that.")
                if (e.status == 404) throw StoryInputException("This story isn't taking answers any more.")
                throw e
            }
            val anonymous = isAnonymousSticker(sticker)
            updateInteraction(story.id) { it.copy(mine = it.mine + (sticker.id to MyAnswer(if (anonymous) null else answer, anonymous))) }
            // Having answered, the totals the author chose to show are shown too.
            runCatching { call { interactions.totals(story) } }.getOrNull()?.let { totals -> updateInteraction(story.id) { it.copy(totals = totals) } }
        } finally {
            updateInteraction(story.id) { it.copy(busy = it.busy - sticker.id) }
        }
    }

    private fun totalsOf(story: Story, inbox: Inbox): Map<String, StickerTotals> =
        story.answerableStickers().associate { it.id to interactions.tally(it, inbox) }

    /** Your own stories with results everyone sees: open the answers and publish the totals, each story at most every few minutes. */
    private suspend fun syncOwnTotals(own: List<Story>, friends: Map<String, StoryFriend>, now: Long) {
        for (story in own) {
            val shows = story.answerableStickers().any { it !is QuestionSticker && resultsOf(it) == ResultsVisibility.Everyone }
            if (!shows || now - (lastTotalsAt[story.id] ?: 0L) < TOTALS_MS) continue
            lastTotalsAt[story.id] = now
            try {
                val inbox = interactions.inbox(identity, story, friends)
                interactions.publishTotals(story, inbox)
                updateInteraction(story.id) { it.copy(inbox = inbox, totals = totalsOf(story, inbox)) }
            } catch (e: ApiException) {
                lastTotalsAt.remove(story.id)
                if (e.status == 401) onSessionExpired()
            }
        }
    }

    private fun resultsOf(s: AnswerableSticker): ResultsVisibility = when (s) {
        is PollSticker -> s.results
        is QuizSticker -> s.results
        is SliderSticker -> s.results
        is QuestionSticker -> ResultsVisibility.Author
    }

    /**
     * A reply, or with [reaction] a quick emoji, to a friend's story: an ordinary end-to-end encrypted chat message to its author that
     * names the story (its id and a short label, never its key). Throws with a message that's safe to show.
     */
    suspend fun reply(story: Story, text: String, reaction: Boolean) {
        if (story.authorId == identity.userId) throw StoryInputException("You can't reply to your own story.")
        if (!story.allowReplies) throw StoryInputException("The author turned replies off for this story.")
        val clean = text.trim()
        if (clean.isEmpty()) throw StoryInputException("Write something first.")
        val ref = StoryReplyRef(story.authorId, story.id, story.kind.wire, story.preview(), reaction)
        val sent = suspendCancellableCoroutine<Boolean> { c -> sendReply(story.authorId, clean, ref) { c.resume(it) } }
        if (!sent) throw StoryInputException("Couldn't send that. Try again.")
    }

    // --- Settings, archive, highlights, reports -------------------------------------------------------------------

    /**
     * Changes your story settings (sealed on this phone, so the server can't read them) and makes what you already posted follow: a person you
     * start hiding stories from loses their keys, someone who leaves your close friends loses the close-friends stories, someone who joins gets
     * them. If another device saved first the change is applied again on top of theirs. Port of updatePrefs in the web app's StoriesContext.
     */
    suspend fun updatePrefs(change: (StoryPrefs) -> StoryPrefs, archive: Boolean? = null) {
        val start = _state.value
        if (!start.on) throw StoryInputException("Stories aren't available right now.")
        if (!start.prefsReadable) throw StoryInputException("Your story settings can't be opened on this phone, so they can't be changed here.")
        val before = start.prefs
        var base = LoadedStoryPrefs(before, readable = true, archiveEnabled = start.archiveEnabled, revision = prefsRevision)
        var saved: LoadedStoryPrefs? = null
        for (attempt in 0 until 3) {
            if (saved != null) break
            try {
                saved = call { repository.savePrefs(identity, change(base.prefs), archive ?: base.archiveEnabled, base.revision) }
            } catch (e: ApiException) {
                if (e.status != 409) throw e
                base = call { repository.loadPrefs(identity) }
                if (!base.readable) throw StoryInputException("Your story settings can't be opened on this phone, so they can't be changed here.")
            }
        }
        val done = saved ?: throw StoryInputException("Couldn't save your story settings. Try again.")
        prefsRevision = done.revision
        _state.update { it.copy(prefs = done.prefs, archiveEnabled = done.archiveEnabled) }
        val after = done.prefs
        for (userId in after.hideFrom - before.hideFrom) runCatching { repository.revokeAccess(userId) }
        val closeStories = _state.value.own.filter { it.meta.closeFriends }
        if (closeStories.isNotEmpty()) {
            for (userId in before.closeFriends - after.closeFriends) runCatching { repository.revokeAccess(userId, closeStories.map { it.id }) }
            for (userId in after.closeFriends - before.closeFriends - after.hideFrom) {
                val friend = _state.value.friends.firstOrNull { it.userId == userId } ?: continue
                for (story in closeStories) runCatching { repository.addKey(identity, story, friend) }
            }
        }
        // Friends you stopped hiding stories from get keys for your "friends" stories on the next top-up.
        if ((before.hideFrom - after.hideFrom).isNotEmpty()) lastGapFillAt = 0
        quietly { refresh() }
    }

    /** Your stories that left your story (kept for you or in a highlight), newest first. */
    suspend fun loadArchive(): List<Story> = call { repository.archive(identity) }

    /** [userId]'s highlights as you can see them (yours, or a friend's). */
    suspend fun highlightsOf(userId: String): List<HighlightView> {
        val key = if (userId == identity.userId) identity.publicKey else _state.value.friends.firstOrNull { it.userId == userId }?.publicKey ?: return emptyList()
        return call { repository.highlights(identity, userId, key) }
    }

    /** Creates a highlight, or with [highlightId] replaces one. */
    suspend fun saveHighlight(highlightId: String?, title: String, stories: List<Story>, cover: Story): String {
        if (stories.isEmpty()) throw StoryInputException("Choose at least one story.")
        if (title.isBlank()) throw StoryInputException("Give the highlight a name.")
        return call { repository.saveHighlight(highlightId, title, stories, cover) }
    }

    suspend fun deleteHighlight(highlightId: String) = call { repository.deleteHighlight(highlightId) }

    /** Reports a story you can see. Nothing from the story goes with it unless [evidence] (what the reporter ticked) is given. */
    suspend fun reportStory(story: Story, reason: String, evidence: String?) {
        if (reason.isBlank()) throw StoryInputException("Say what is wrong.")
        try {
            call { repository.reportStory(story.id, reason.trim().take(300), evidence?.take(600)) }
        } catch (e: ApiException) {
            if (e.code == "already_reported") throw StoryInputException("You already reported this story.")
            throw e
        }
    }

    /** The author reports an abusive answer (even one sent without a name): admins see who sent it, the author doesn't. */
    suspend fun reportAnswer(story: Story, entry: com.redvaultsystems.redvaultmessage.data.model.InboxEntry, reason: String, evidence: String?) {
        if (reason.isBlank()) throw StoryInputException("Say what is wrong.")
        try {
            call { interactions.reportAnswer(story.id, entry.sticker.id, entry.envelope, reason.trim().take(300), evidence?.take(600)) }
        } catch (e: ApiException) {
            if (e.code == "already_reported" || e.status == 409) throw StoryInputException("You already reported this answer.")
            throw e
        }
    }

    // A list the viewer is about to open (the archive, a highlight): held here because the route only carries a name.
    private var pendingList: List<ViewerGroup>? = null

    fun openList(groups: List<ViewerGroup>) {
        pendingList = groups
    }

    fun takeList(): List<ViewerGroup> = pendingList.orEmpty()

    /** The handles of friends who shared that they saw [story] (yours). */
    suspend fun viewers(story: Story): List<String> {
        val friends = _state.value.friends.associateBy { it.userId }
        return call { repository.viewers(story.id) }.map { friends[it]?.handle ?: "Former friend" }
    }

    /** Your own user id. */
    fun selfId(): String = identity.userId

    /** Wipes the keys and the decrypted media. Called when the session ends. */
    fun close() {
        _state.value.own.forEach { it.wipe() }
        allAuthors.forEach { a -> a.stories.forEach { it.wipe() } }
        allAuthors = emptyList()
        repository.clearCache()
        interactions.forgetPublished()
        _interactions.value = emptyMap()
        lastTotalsAt.clear()
        pendingList = null
        composer = null
        _state.value = StoriesState()
    }

    private suspend fun <T> call(block: suspend () -> T): T {
        try {
            return block()
        } catch (e: ApiException) {
            if (e.status == 401) onSessionExpired()
            throw e
        }
    }

    private suspend fun quietly(block: suspend () -> Unit) {
        try {
            block()
        } catch (e: CancellationException) {
            throw e
        } catch (_: Exception) {
        }
    }

    private fun Story.withSeen() = Story(id, authorId, kind, meta, key, createdAtMs, expiresAtMs, allowReplies, hasMedia, seen = true, viewCount = viewCount)

    private companion object {
        const val REFRESH_MS = 60_000L
        const val CONFIG_RETRY_MS = 30_000L
        const val CONFIG_OFF_MS = 5 * 60_000L
        const val CONFIG_ON_MS = 10 * 60_000L
        const val PREFS_MS = 5 * 60_000L
        const val GAP_FILL_MS = 5 * 60_000L
        const val TOTALS_MS = 3 * 60_000L
    }
}

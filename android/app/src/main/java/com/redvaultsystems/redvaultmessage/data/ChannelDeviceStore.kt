package com.redvaultsystems.redvaultmessage.data

import android.content.Context
import com.redvaultsystems.redvaultmessage.data.model.ChannelPosts
import com.redvaultsystems.redvaultmessage.data.model.RememberedVote
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.builtins.MapSerializer
import kotlinx.serialization.builtins.serializer
import kotlinx.serialization.json.Json

/**
 * What this phone remembers about channels, and never sends anywhere: which
 * posts it already counted a view for (phone-wide, like the web's
 * per-browser list, so each device counts a post once), your poll votes,
 * where you stopped in a podcast, and which new posts you were already
 * notified of. Excluded from backups like everything else the app keeps.
 */
class ChannelDeviceStore(context: Context) {
    private val prefs = context.getSharedPreferences("channelDevice", Context.MODE_PRIVATE)
    private val ids = ListSerializer(String.serializer())
    private val votes = MapSerializer(String.serializer(), RememberedVote.serializer())
    private val positions = MapSerializer(String.serializer(), Int.serializer())
    private val notified = MapSerializer(String.serializer(), String.serializer())

    /** Of [postIds], those this phone hasn't counted yet (lowercased, each once), now remembered as counted. */
    @Synchronized
    fun takeUncountedViews(postIds: List<String>): List<String> {
        val remembered = read(KEY_VIEWS, ids, emptyList()).toMutableList()
        val fresh = ChannelPosts.uncountedViews(remembered, postIds)
        if (fresh.isNotEmpty()) write(KEY_VIEWS, ids, remembered)
        return fresh
    }

    fun vote(userId: String, postId: String): RememberedVote? = read(votesKey(userId), votes, emptyMap())[postId.lowercase()]

    /** Null or an empty choice forgets it (a vote taken back). Keeps the most recent 2000. */
    @Synchronized
    fun rememberVote(userId: String, postId: String, vote: RememberedVote?) {
        val all = LinkedHashMap(read(votesKey(userId), votes, emptyMap()))
        all.remove(postId.lowercase())
        if (vote != null && vote.mine.isNotEmpty()) all[postId.lowercase()] = vote
        write(votesKey(userId), votes, all.entries.toList().takeLast(MAX_VOTES).associate { it.key to it.value })
    }

    /** Where you stopped in a podcast or long audio, in seconds (0 when you'd finished or never started). */
    fun position(mediaId: String): Int = read(KEY_POSITIONS, positions, emptyMap())[mediaId] ?: 0

    @Synchronized
    fun savePosition(mediaId: String, seconds: Int) {
        val all = LinkedHashMap(read(KEY_POSITIONS, positions, emptyMap()))
        all.remove(mediaId)
        if (seconds > 0) all[mediaId] = seconds
        write(KEY_POSITIONS, positions, all.entries.toList().takeLast(MAX_POSITIONS).associate { it.key to it.value })
    }

    /** Per followed channel, the newest post (its createdAt) you were notified of. */
    fun notified(userId: String): Map<String, String> = read(notifiedKey(userId), notified, emptyMap())

    fun saveNotified(userId: String, value: Map<String, String>) = write(notifiedKey(userId), notified, value)

    /** Whether the Channels tab already offered to turn on notifications. */
    var askedForNotifications: Boolean
        get() = prefs.getBoolean(KEY_ASKED, false)
        set(value) = prefs.edit().putBoolean(KEY_ASKED, value).apply()

    private fun votesKey(userId: String) = "votes:$userId"
    private fun notifiedKey(userId: String) = "notified:$userId"

    private fun <T> read(key: String, serializer: kotlinx.serialization.KSerializer<T>, fallback: T): T =
        prefs.getString(key, null)?.let { runCatching { json.decodeFromString(serializer, it) }.getOrNull() } ?: fallback

    private fun <T> write(key: String, serializer: kotlinx.serialization.KSerializer<T>, value: T) =
        prefs.edit().putString(key, json.encodeToString(serializer, value)).apply()

    private companion object {
        val json = Json { ignoreUnknownKeys = true }
        const val KEY_VIEWS = "views"
        const val KEY_POSITIONS = "positions"
        const val KEY_ASKED = "askedForNotifications"
        const val MAX_VOTES = 2000
        const val MAX_POSITIONS = 500
    }
}

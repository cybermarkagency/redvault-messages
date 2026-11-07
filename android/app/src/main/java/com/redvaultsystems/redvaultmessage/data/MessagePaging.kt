package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.data.api.MessageDto
import java.time.Instant

/**
 * Port of the desktop's fetchLatest (desktop/core/src/data/conversation_repository.cpp): a chat's
 * newest [window] messages, fetched a page at a time with `before`. An open chat's window grows by
 * [PAGE_SIZE] each time older messages are asked for; every refresh fetches the whole window again,
 * so edits, reactions and deletions in older messages show too.
 */
object MessagePaging {
    /** What a chat shows at first, and how many more each "older" step adds (the server's default page). */
    const val PAGE_SIZE = 50

    /** The most the server returns in one page. */
    const val MAX_PAGE = 200

    class Latest(
        /** Newest first, at most the window asked for. */
        val messages: List<MessageDto>,
        /** The last page was full: there may be older messages than these. */
        val hasOlder: Boolean,
    )

    suspend fun fetchLatest(window: Int, page: suspend (limit: Int, before: String?) -> List<MessageDto>): Latest {
        val want = window.coerceAtLeast(1)
        val out = ArrayList<MessageDto>()
        val seen = HashSet<String>()
        var before: String? = null
        // Messages already held from the oldest millisecond fetched so far: the next page repeats them.
        var overlap = 0
        var hasOlder = false
        while (out.size < want) {
            val limit = minOf(MAX_PAGE, want - out.size + overlap)
            val got = page(limit, before)
            val full = got.size >= limit
            var fresh = 0
            for (dto in got) {
                if (seen.add(dto.id)) {
                    out += dto
                    fresh++
                }
            }
            hasOlder = full
            // A short page is the start of the chat; a page with nothing new would only loop.
            if (!full || fresh == 0) break
            // One millisecond later than the oldest: the server compares finer than milliseconds, so this
            // re-asks for that oldest moment (dropped above as duplicates) rather than skipping a message
            // created in the same millisecond.
            val oldest = out.minOf { ms(it) }
            overlap = out.count { ms(it) == oldest }
            before = Instant.ofEpochMilli(oldest + 1).toString()
        }
        // The overlap can bring in a few more than asked for; keep the newest [want].
        return Latest(out.sortedByDescending(::ms).take(want), hasOlder)
    }

    private fun ms(dto: MessageDto): Long = Instant.parse(dto.createdAt).toEpochMilli()
}

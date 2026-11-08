// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Edits, forwards, pins, polls and formatting: the same wire format, rules
 * and expected strings as ui/src/test/richFeatures.test.ts (and the desktop
 * app's message_format_test.cpp, conversation_repository_test.cpp and
 * text_format_test.cpp).
 */
class RichFeaturesTest {
    private val msg = "3f2504e0-4f89-11d3-9a0c-0305e82c3301"
    private val msg2 = "9b2e6a3c-1d4f-4e7a-8b5c-2a1f0e9d8c7b"

    @Test
    fun newFieldsAreByteForByteWhatTheOtherAppsSend() {
        assertEquals("\u0000rv-msg:{\"type\":\"text\",\"version\":1,\"text\":\"fixed typo\",\"edits\":\"$msg\"}", RichMessage.serializeEdit("fixed typo", msg))
        assertEquals("\u0000rv-msg:{\"type\":\"text\",\"version\":1,\"text\":\"look at this\",\"forwarded\":true}", RichMessage.serializeForwarded("look at this"))
        assertEquals(
            "\u0000rv-msg:{\"type\":\"text\",\"version\":1,\"text\":\"📌 Pinned: note 1\",\"pin\":{\"messageId\":\"$msg\",\"preview\":\"note 1\"}}",
            RichMessage.serializePin(msg, "note 1"),
        )
        assertEquals("\u0000rv-msg:{\"type\":\"text\",\"version\":1,\"text\":\"Unpinned a message\",\"unpin\":\"$msg\"}", RichMessage.serializeUnpin(msg))
        assertEquals(
            "\u0000rv-msg:{\"type\":\"text\",\"version\":1,\"text\":\"📊 Poll: Where?\\n1. Park\\n2. Beach\",\"poll\":{\"question\":\"Where?\",\"options\":[\"Park\",\"Beach\"],\"multiple\":false}}",
            RichMessage.serializePoll("Where?", listOf("Park", " Beach "), false),
        )
        assertEquals(
            "\u0000rv-msg:{\"type\":\"text\",\"version\":1,\"text\":\"Voted: Beach\",\"vote\":{\"pollId\":\"$msg\",\"options\":[1]}}",
            RichMessage.serializeVote(msg, listOf(1), listOf("Park", "Beach")),
        )
        assertEquals("\u0000rv-msg:{\"type\":\"text\",\"version\":1,\"text\":\"Ended the poll\",\"endPoll\":\"$msg\"}", RichMessage.serializeEndPoll(msg))
    }

    @Test
    fun parsesBackWithIdsLowercased() {
        assertEquals(ParsedBody.Text("x", null, edits = msg), RichMessage.parse(RichMessage.serializeEdit("x", msg.uppercase())))
        assertEquals(ParsedBody.Text("x", null, forwarded = true), RichMessage.parse(RichMessage.serializeForwarded("x")))
        assertEquals(PinRef(msg, "hi"), (RichMessage.parse(RichMessage.serializePin(msg, "hi")) as ParsedBody.Text).pin)
        assertEquals(msg, (RichMessage.parse(RichMessage.serializeUnpin(msg)) as ParsedBody.Text).unpin)
        assertEquals(PollDef("Q", listOf("a", "b"), true), (RichMessage.parse(RichMessage.serializePoll("Q", listOf("a", "b"), true)) as ParsedBody.Text).poll)
        assertEquals(VoteRef(msg, listOf(0, 2)), (RichMessage.parse(RichMessage.serializeVote(msg, listOf(0, 0, 2), emptyList())) as ParsedBody.Text).vote)
        assertEquals(msg, (RichMessage.parse(RichMessage.serializeEndPoll(msg)) as ParsedBody.Text).endPoll)
    }

    @Test
    fun rejectsMalformedFieldsInsteadOfHalfApplyingThem() {
        fun body(extra: String) = "\u0000rv-msg:{\"type\":\"text\",\"version\":1,\"text\":\"x\",$extra}"
        assertEquals(ParsedBody.Invalid, RichMessage.parse(body("\"edits\":\"nope\"")))
        assertEquals(ParsedBody.Invalid, RichMessage.parse(body("\"forwarded\":\"yes\"")))
        assertEquals(ParsedBody.Invalid, RichMessage.parse(body("\"poll\":{\"question\":\"Q\",\"options\":[\"only one\"]}")))
        assertEquals(ParsedBody.Invalid, RichMessage.parse(body("\"poll\":{\"question\":\"Q\",\"options\":[\"a\",2]}")))
        assertEquals(ParsedBody.Invalid, RichMessage.parse(body("\"vote\":{\"pollId\":\"$msg\",\"options\":[99]}")))
        assertEquals(ParsedBody.Invalid, RichMessage.parse(body("\"vote\":{\"pollId\":\"$msg\",\"options\":[\"1\"]}")))
        assertEquals(ParsedBody.Invalid, RichMessage.parse(body("\"pin\":[]")))
        assertTrue(runCatching { RichMessage.serializePoll("Q", listOf("same", "same"), false) }.isFailure)
        assertTrue(runCatching { RichMessage.serializePoll("  ", listOf("a", "b"), false) }.isFailure)
    }

    // The same cases as ui/src/test/richFeatures.test.ts and desktop/tests/message_format_test.cpp.
    private val zwj = "‍"

    @Test
    fun anySingleEmojiIsAReaction() {
        val one = listOf(
            "👍", "❤️", "💩", "👍🏽", "👨${zwj}👩${zwj}👧", "🏳️${zwj}🌈", "🇳🇱", "1️⃣", "#⃣",
            "🏴" + listOf(0xE0067, 0xE0062, 0xE0073, 0xE0063, 0xE0074, 0xE007F).joinToString("") { String(Character.toChars(it)) },
            "👩🏽${zwj}❤️${zwj}💋${zwj}👨🏻", "☺️", "©️",
        )
        for (emoji in one) {
            assertTrue(emoji, RichMessage.isReactionEmoji(emoji))
            assertEquals(ParsedBody.Reaction(msg, emoji), RichMessage.parse(RichMessage.serializeReaction(msg, emoji)))
        }
        // At most 16 code points.
        val fifteen = "👍$zwj".repeat(7) + "👍"
        assertTrue(RichMessage.isReactionEmoji(fifteen))
        assertFalse(RichMessage.isReactionEmoji(fifteen + zwj + "👍"))
    }

    @Test
    fun nothingElseIsAReaction() {
        val notOne = listOf("", "x", "👍👍", "👍 ", " 👍", "🇳", "🏽", "${zwj}👍", "👍$zwj", "1", "11⃣", "👍\n", "🇳🇱🇳🇱", "\uD83D")
        for (text in notOne) assertFalse(text, RichMessage.isReactionEmoji(text))
        assertTrue(runCatching { RichMessage.serializeReaction(msg, "👍👍") }.isFailure)
    }

    @Test
    fun showsTheQuickSixFirstThenTheRestInTheOrderFirstUsed() {
        fun tally(firstAt: Long, vararg senders: String, mine: String? = null) =
            ReactionTally(firstAt).apply { this.senders += senders; myReactionId = mine }
        val ordered = MessageFolding.orderReactions(
            mapOf("🦄" to tally(5, "a"), "🙏" to tally(9, "a", "b", mine = "r1"), "🎉" to tally(2, "b"), "👍" to tally(7, "c")),
        )
        assertEquals(listOf("👍", "🙏", "🎉", "🦄"), ordered.map { it.emoji })
        assertEquals(ReactionView("🙏", 2, true, "r1"), ordered[1])
    }

    @Test
    fun leavesPlainTextAlone() {
        assertFalse(TextFormat.hasFormatting("snake_case_name and 2*3*4 and file_name.txt"))
        assertFalse(TextFormat.hasFormatting("**double** and * spaced * and *never closed"))
    }

    @Test
    fun findsMarkersMonospaceSpoilersAndLinks() {
        assertEquals(
            listOf(
                FormatNode.Plain("a "),
                FormatNode.Styled(FormatStyle.Bold, listOf(FormatNode.Plain("bold"))),
                FormatNode.Plain(" and "),
                FormatNode.Styled(FormatStyle.Italic, listOf(FormatNode.Plain("it"))),
                FormatNode.Plain(" "),
                FormatNode.Styled(FormatStyle.Strike, listOf(FormatNode.Plain("s"))),
            ),
            TextFormat.parse("a *bold* and _it_ ~s~"),
        )
        assertEquals(listOf(FormatNode.Code("*not bold*")), TextFormat.parse("`*not bold*`"))
        assertEquals(
            listOf(FormatNode.Styled(FormatStyle.Bold, listOf(FormatNode.Plain("bold and "), FormatNode.Styled(FormatStyle.Italic, listOf(FormatNode.Plain("italic")))))),
            TextFormat.parse("*bold and _italic_*"),
        )
        assertEquals(listOf(FormatNode.Styled(FormatStyle.Spoiler, listOf(FormatNode.Plain("the butler")))), TextFormat.parse("||the butler||"))
        assertEquals(listOf(FormatNode.Plain("*one\ntwo*")), TextFormat.parse("*one\ntwo*"))
        assertEquals(
            listOf(FormatNode.Plain("see "), FormatNode.Link("https://example.com/a?b=1"), FormatNode.Plain(".")),
            TextFormat.parse("see https://example.com/a?b=1."),
        )
        assertEquals(
            listOf(FormatNode.Plain("("), FormatNode.Link("http://example.com/x_(y)"), FormatNode.Plain(")")),
            TextFormat.parse("(http://example.com/x_(y))"),
        )
        assertFalse(TextFormat.hasFormatting("javascript:alert(1) file:///c:/x ftp://x xhttps://example.com"))
    }

    @Test
    fun highlightsGroupMembersMentionsYoursMore() {
        val nodes = TextFormat.parse("hi @Bob and @bobby, email@bob.com @me @carol", listOf("bob", "bobby", "me"), "me")
        assertTrue(FormatNode.Mention("@Bob", false) in nodes)
        assertTrue(FormatNode.Mention("@bobby", false) in nodes)
        assertTrue(FormatNode.Mention("@me", true) in nodes)
        assertEquals(3, nodes.count { it !is FormatNode.Plain })
        assertTrue(TextFormat.mentions("ping @me please", "me"))
        assertFalse(TextFormat.mentions("ping @meg", "me"))
    }

    @Test
    fun dropsMarkersAndHidesSpoilersInPreviews() {
        assertEquals("bold and it s c •••", TextFormat.toPlain("*bold* and _it_ ~s~ `c` ||secret||"))
    }

    @Test
    fun staysFastOnHostileInput() {
        val start = System.currentTimeMillis()
        TextFormat.parse(" *a".repeat(20000))
        TextFormat.parse("*_".repeat(5000) + "x" + "_*".repeat(5000))
        assertTrue(System.currentTimeMillis() - start < 2000)
    }

    private fun message(id: String, fromMe: Boolean, text: String, at: Long, poll: PollView? = null, notice: NoticeView? = null) =
        Message(id = id, fromMe = fromMe, kind = MessageKind.Text, text = text, createdAtMs = at, deleteAtMs = null, poll = poll, notice = notice)

    @Test
    fun appliesOnlyTheOriginalSendersEdits() {
        val folded = MessageFolding.foldAll(
            listOf(message(msg, false, "see you at 5", 1)),
            listOf(
                EditEvent("e1", msg, "bob-id", "see you at 6", 2),
                EditEvent("e2", msg, "alice-id", "forged", 3),
                EditEvent("e3", msg, "bob-id", "see you at 7", 4),
            ),
            emptyList(),
            mapOf(msg to "bob-id"),
            "alice-id",
        ).single()
        assertEquals("see you at 7", folded.text)
        assertEquals(4L, folded.editedAtMs)
        assertEquals(listOf("see you at 5", "see you at 6"), folded.earlierVersions.map { it.text })
        assertEquals(listOf("e1", "e3"), folded.editMessageIds)
    }

    @Test
    fun talliesEachPersonsLatestVoteUntilTheCreatorEndsThePoll() {
        val poll = message(msg, false, "Where?", 1, poll = PollView("Where?", listOf("Park", "Beach", "Cafe"), multiple = false))
        val folded = MessageFolding.foldAll(
            listOf(poll),
            emptyList(),
            listOf(
                PollEvent(msg, "alice-id", 2, false, listOf(0)),
                PollEvent(msg, "carol-id", 3, false, listOf(1)),
                PollEvent(msg, "alice-id", 4, false, listOf(2, 0)),
                PollEvent(msg, "carol-id", 5, true, emptyList()),
                PollEvent(msg, "bob-id", 6, true, emptyList()),
                PollEvent(msg, "carol-id", 7, false, listOf(2)),
            ),
            mapOf(msg to "bob-id"),
            "alice-id",
        ).single().poll!!
        assertTrue(folded.closed)
        assertEquals(listOf(0, 1, 1), folded.counts)
        assertEquals(listOf(2), folded.mine)
        assertEquals(2, folded.voters)
    }

    @Test
    fun dropsAPinOnceItsMessageIsGone() {
        val notices = listOf(
            message("n1", false, "a", 1, notice = NoticeView(NoticeKind.Pin, msg)),
            message("n2", false, "b", 2, notice = NoticeView(NoticeKind.Pin, msg2, targetGone = true)),
        )
        assertEquals(listOf(msg), MessageFolding.pinnedFrom(notices).map { it.messageId })
    }

    @Test
    fun keepsTheNewestThreePins() {
        val notices = listOf(msg, msg2, "a", "b").mapIndexed { i, target ->
            message("n$i", true, "note $i", i.toLong(), notice = NoticeView(NoticeKind.Pin, target))
        } + message("n9", false, "", 9, notice = NoticeView(NoticeKind.Unpin, "a"))
        assertEquals(listOf(msg2, "b"), MessageFolding.pinnedFrom(notices).map { it.messageId })
    }
}

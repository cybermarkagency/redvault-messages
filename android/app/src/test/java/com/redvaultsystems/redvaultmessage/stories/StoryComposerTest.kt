// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.stories

import com.redvaultsystems.redvaultmessage.data.DraftContent
import com.redvaultsystems.redvaultmessage.data.DraftSlide
import com.redvaultsystems.redvaultmessage.data.PreparedStoryMedia
import com.redvaultsystems.redvaultmessage.data.SlideMode
import com.redvaultsystems.redvaultmessage.data.StoryAudience
import com.redvaultsystems.redvaultmessage.data.StoryDraftStore
import com.redvaultsystems.redvaultmessage.data.model.CountdownSticker
import com.redvaultsystems.redvaultmessage.data.model.DateSticker
import com.redvaultsystems.redvaultmessage.data.model.DateStyle
import com.redvaultsystems.redvaultmessage.data.model.LinkSticker
import com.redvaultsystems.redvaultmessage.data.model.LocationSticker
import com.redvaultsystems.redvaultmessage.data.model.MentionSticker
import com.redvaultsystems.redvaultmessage.data.model.PollSticker
import com.redvaultsystems.redvaultmessage.data.model.QuestionSticker
import com.redvaultsystems.redvaultmessage.data.model.QuizSticker
import com.redvaultsystems.redvaultmessage.data.model.ResultsVisibility
import com.redvaultsystems.redvaultmessage.data.model.SliderSticker
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StorySticker
import com.redvaultsystems.redvaultmessage.ui.stories.ComposerModel
import com.redvaultsystems.redvaultmessage.ui.stories.Selected
import com.redvaultsystems.redvaultmessage.ui.stories.SlideUi
import com.redvaultsystems.redvaultmessage.ui.stories.StickerFields
import com.redvaultsystems.redvaultmessage.ui.stories.StickerKind
import com.redvaultsystems.redvaultmessage.ui.stories.StoryTiming
import com.redvaultsystems.redvaultmessage.ui.stories.buildSticker
import com.redvaultsystems.redvaultmessage.ui.stories.lifetimeLabel
import com.redvaultsystems.redvaultmessage.ui.stories.newStickerId
import com.redvaultsystems.redvaultmessage.ui.stories.withPlacement
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream

/** What the story composer decides without a screen: the viewer's clock, the stickers it makes, and the slides it holds. */
class StoryComposerTest {
    private val now = 1_800_000_000_000L
    private val userId = "00000000-0000-4000-8000-0000000000a1"

    private fun make(kind: StickerKind, f: StickerFields) = buildSticker(kind, f, now, "stickerid01")

    // --- The viewer's clock ---------------------------------------------------------------------------------------

    private val link = LinkSticker("linksticker1", 0.5, 0.5, 1.0, 0.0, "https://example.com/", "Link")
    private val poll = PollSticker("pollsticker1", 0.5, 0.5, 1.0, 0.0, "Q?", listOf("a", "b"), null, ResultsVisibility.Everyone)

    @Test
    fun aStoryStaysFiveSecondsOrTwentyWhenSomethingWaitsForAnAnswer() {
        assertEquals(5_000L, StoryTiming.stillMs(emptyList()))
        // Information alone doesn't need time to answer.
        assertEquals(5_000L, StoryTiming.stillMs(listOf(link, CountdownSticker("count1234", 0.5, 0.5, 1.0, 0.0, "Party", now + 1000), LocationSticker("place1234", 0.5, 0.5, 1.0, 0.0, "Rome"))))
        assertEquals(20_000L, StoryTiming.stillMs(listOf(poll)))
        assertEquals(20_000L, StoryTiming.stillMs(listOf(link, QuestionSticker("question1", 0.5, 0.5, 1.0, 0.0, "Ask", false))))
        assertEquals(20_000L, StoryTiming.stillMs(listOf(SliderSticker("slider123", 0.5, 0.5, 1.0, 0.0, "How?", "🔥", ResultsVisibility.Author))))
        assertEquals(20_000L, StoryTiming.stillMs(listOf(QuizSticker("quiz12345", 0.5, 0.5, 1.0, 0.0, "Q?", listOf("a", "b"), 1, null, ResultsVisibility.Everyone))))
    }

    @Test
    fun theBarFillsByTheTimeSpentAndAVideoBarFollowsThePlayer() {
        assertEquals(0.5f, StoryTiming.advance(0f, 2_500_000_000L, 5_000L), 0.0001f)
        assertEquals(0.25f, StoryTiming.advance(0.2f, 250_000_000L, 5_000L), 0.0001f)
        assertEquals(1.0f, StoryTiming.advance(0f, 20_000_000_000L, 20_000L), 0.0001f)
        assertEquals(0f, StoryTiming.advance(0f, 0L, 5_000L), 0f)
        assertEquals(0.5f, StoryTiming.videoProgress(1_000, 2_000)!!, 0.0001f)
        assertEquals(1f, StoryTiming.videoProgress(2_500, 2_000)!!, 0f)
        assertEquals(0f, StoryTiming.videoProgress(-5, 2_000)!!, 0f)
        assertNull("a length that isn't known yet moves nothing", StoryTiming.videoProgress(500, 0))
        assertNull(StoryTiming.videoProgress(500, -1))
    }

    @Test
    fun lifetimesAreNamedTheWayTheServerOffersThem() {
        assertEquals("1 hour", lifetimeLabel(3_600))
        assertEquals("6 hours", lifetimeLabel(21_600))
        assertEquals("24 hours", lifetimeLabel(86_400))
        assertEquals("2 days", lifetimeLabel(172_800))
        assertEquals("30 minutes", lifetimeLabel(1_800))
    }

    // --- Stickers the composer makes ---------------------------------------------------------------------------------

    @Test
    fun everyKindOfStickerIsMadeAndReadBackByTheSameRulesEveryAppUses() {
        val poll = make(StickerKind.Poll, StickerFields(question = "Pizza?", options = listOf("Yes", "No"), onlyMe = true, deadlineMs = 3_600_000)) as PollSticker
        assertEquals("Pizza?", poll.question)
        assertEquals(listOf("Yes", "No"), poll.options)
        assertEquals(ResultsVisibility.Author, poll.results)
        assertEquals(now + 3_600_000, poll.closesAt)
        assertEquals(listOf(0.5, 0.5, 1.0, 0.0), listOf(poll.x, poll.y, poll.scale, poll.rotation))
        assertEquals("stickerid01", poll.id)
        assertNull((make(StickerKind.Poll, StickerFields(question = "Pizza?", options = listOf("Yes", "No", "Maybe"))) as PollSticker).closesAt)

        val quiz = make(StickerKind.Quiz, StickerFields(question = "2 + 2?", options = listOf("3", "4", "5"), correct = 1, explanation = "Arithmetic")) as QuizSticker
        assertEquals(1, quiz.correct)
        assertEquals("Arithmetic", quiz.explanation)
        assertEquals(ResultsVisibility.Everyone, quiz.results)
        assertNull((make(StickerKind.Quiz, StickerFields(question = "2 + 2?", options = listOf("3", "4"), correct = 0, explanation = "  ")) as QuizSticker).explanation)

        val question = make(StickerKind.Question, StickerFields(question = "Ask me anything", anonymous = true)) as QuestionSticker
        assertEquals("Ask me anything", question.prompt)
        assertTrue(question.anonymous)
        assertFalse((make(StickerKind.Question, StickerFields(question = "Ask me anything")) as QuestionSticker).anonymous)

        val slider = make(StickerKind.Slider, StickerFields(question = "How much?", emoji = "🔥", onlyMe = true)) as SliderSticker
        assertEquals("🔥", slider.emoji)
        assertEquals(ResultsVisibility.Author, slider.results)

        val countdown = make(StickerKind.Countdown, StickerFields(question = "Party", durationMs = 86_400_000)) as CountdownSticker
        assertEquals("Party", countdown.title)
        assertEquals(now + 86_400_000, countdown.endsAt)

        val link = make(StickerKind.Link, StickerFields(url = "  https://Example.com/a?b=1 ", label = "Details")) as LinkSticker
        assertEquals("https://example.com/a?b=1", link.url) // in the form the parser writes it
        assertEquals("Details", link.label)

        val mention = make(StickerKind.Mention, StickerFields(mention = userId)) as MentionSticker
        assertEquals(userId, mention.userId)

        val date = make(StickerKind.Date, StickerFields(whenMs = now + 5_000, dateStyle = "date")) as DateSticker
        assertEquals(now + 5_000, date.at)
        assertEquals(DateStyle.Date, date.style)
        assertEquals(DateStyle.DateTime, (make(StickerKind.Date, StickerFields(whenMs = now + 5_000)) as DateSticker).style)

        val place = make(StickerKind.Place, StickerFields(label = "Café Roma")) as LocationSticker
        assertEquals("Café Roma", place.label)
    }

    @Test
    fun aStickerAnotherAppWouldDropIsNeverMade() {
        val two = listOf("Yes", "No")
        // Polls and quizzes: a question, two to four options, each filled and short; a quiz needs a real right answer.
        assertNull(make(StickerKind.Poll, StickerFields(question = "", options = two)))
        assertNull(make(StickerKind.Poll, StickerFields(question = " ", options = two)))
        assertNull(make(StickerKind.Poll, StickerFields(question = "Q?", options = listOf("Yes", ""))))
        assertNull(make(StickerKind.Poll, StickerFields(question = "Q?", options = listOf("Yes", "x".repeat(41)))))
        assertNull(make(StickerKind.Poll, StickerFields(question = "x".repeat(121), options = two)))
        assertNull(make(StickerKind.Poll, StickerFields(question = "Q?", options = listOf("Only"))))
        assertNull(make(StickerKind.Poll, StickerFields(question = "Q?", options = listOf("a", "b", "c", "d", "e"))))
        assertNotNull(make(StickerKind.Poll, StickerFields(question = "x".repeat(120), options = listOf("x".repeat(40), "b", "c", "d"))))
        assertNull(make(StickerKind.Quiz, StickerFields(question = "Q?", options = two, correct = 2)))
        assertNull(make(StickerKind.Quiz, StickerFields(question = "Q?", options = two, correct = -1)))
        assertNotNull(make(StickerKind.Quiz, StickerFields(question = "Q?", options = two, correct = 1)))
        // Questions and sliders.
        assertNull(make(StickerKind.Question, StickerFields(question = "")))
        assertNull(make(StickerKind.Slider, StickerFields(question = "How?", emoji = "")))
        assertNull(make(StickerKind.Slider, StickerFields(question = "", emoji = "🔥")))
        assertNull(make(StickerKind.Slider, StickerFields(question = "How?", emoji = "x".repeat(9))))
        // Countdown, date and place.
        assertNull(make(StickerKind.Countdown, StickerFields(question = "", durationMs = 1_000)))
        assertNull(make(StickerKind.Countdown, StickerFields(question = "x".repeat(61), durationMs = 1_000)))
        assertNull(make(StickerKind.Date, StickerFields(whenMs = 0)))
        assertNull(make(StickerKind.Date, StickerFields(whenMs = now, dateStyle = "weekday")))
        assertNull(make(StickerKind.Place, StickerFields(label = " ")))
        assertNull(make(StickerKind.Place, StickerFields(label = "x".repeat(61))))
        // Links: https only, no credentials, a real address, a label, short enough.
        assertNull(make(StickerKind.Link, StickerFields(url = "http://example.com", label = "x")))
        assertNull(make(StickerKind.Link, StickerFields(url = "javascript:alert(1)", label = "x")))
        assertNull(make(StickerKind.Link, StickerFields(url = "https://user:pass@example.com", label = "x")))
        assertNull(make(StickerKind.Link, StickerFields(url = "https://localhost", label = "x")))
        assertNull(make(StickerKind.Link, StickerFields(url = "https://example.com", label = "")))
        assertNull(make(StickerKind.Link, StickerFields(url = "https://example.com/" + "a".repeat(290), label = "x")))
        assertNull(make(StickerKind.Link, StickerFields(url = "https://", label = "x")))
        // Mentions need a friend picked.
        assertNull(make(StickerKind.Mention, StickerFields(mention = null)))
        assertNull(make(StickerKind.Mention, StickerFields(mention = "someone")))
    }

    @Test
    fun theKindsThatTakeAnswersAreTheOnesTheServerMustAllow() {
        assertEquals(
            setOf(StickerKind.Poll, StickerKind.Quiz, StickerKind.Question, StickerKind.Slider),
            StickerKind.entries.filter { it.interactive }.toSet(),
        )
        assertEquals(9, StickerKind.entries.size)
    }

    @Test
    fun newStickerIdsAreTwelveLowercaseLettersAndDigitsAndDoNotRepeat() {
        val ids = (1..200).map { newStickerId() }
        assertTrue(ids.all { Regex("^[a-z0-9]{12}$").matches(it) })
        assertEquals(200, ids.toSet().size)
        // The parser accepts them (it takes 6 to 16 of those characters).
        assertNotNull(buildSticker(StickerKind.Place, StickerFields(label = "Rome"), now, ids.first()))
    }

    @Test
    fun movingAStickerChangesOnlyWhereItIsAndHowBigAndTurned() {
        val all: List<StorySticker> = listOf(
            poll,
            QuizSticker("quiz12345", 0.1, 0.2, 1.0, 0.0, "Q?", listOf("a", "b"), 1, "why", ResultsVisibility.Author),
            QuestionSticker("question1", 0.1, 0.2, 1.0, 0.0, "Ask", true),
            SliderSticker("slider123", 0.1, 0.2, 1.0, 0.0, "How?", "🔥", ResultsVisibility.Author),
            CountdownSticker("count1234", 0.1, 0.2, 1.0, 0.0, "Party", now),
            link,
            MentionSticker("mention12", 0.1, 0.2, 1.0, 0.0, userId),
            DateSticker("date12345", 0.1, 0.2, 1.0, 0.0, now, DateStyle.Time),
            LocationSticker("place1234", 0.1, 0.2, 1.0, 0.0, "Rome"),
        )
        for (s in all) {
            val moved = withPlacement(s, 0.8, 0.3, 2.0, 45.0)
            assertEquals(s::class, moved::class)
            assertEquals(s.id, moved.id)
            assertEquals(listOf(0.8, 0.3, 2.0, 45.0), listOf(moved.x, moved.y, moved.scale, moved.rotation))
            // Everything else about it is as it was.
            assertEquals(withPlacement(s, 0.0, 0.0, 1.0, 0.0), withPlacement(moved, 0.0, 0.0, 1.0, 0.0))
        }
    }

    // --- The slides the composer holds -----------------------------------------------------------------------------

    private fun photo() = PreparedStoryMedia(StoryKind.Image, "image/jpeg", 640, 480, null, 3) { ByteArrayInputStream(byteArrayOf(1, 2, 3)) }

    @Test
    fun aNewComposerHoldsOneEmptySlide() {
        val model = ComposerModel(defaultReplies = true)
        assertEquals(1, model.slides.size)
        assertEquals(0, model.current)
        assertFalse(model.hasWork())
        assertFalse(model.dirty)
        assertTrue(model.replies)
        assertEquals(StoryAudience.Friends, model.audience)
        assertFalse(ComposerModel(defaultReplies = false).replies)
    }

    @Test
    fun editingMarksItDirtyAndOnlyChangesTheSlideBeingEdited() {
        val model = ComposerModel(true)
        model.addSlide()
        model.select(0)
        model.change { it.copy(text = "first") }
        assertTrue(model.dirty)
        assertTrue(model.hasWork())
        assertEquals(listOf("first", ""), model.slides.map { it.text })
        model.select(1)
        model.change { it.copy(text = "second", background = StoryBackground.Night, font = StoryFont.Strong) }
        assertEquals(listOf("first", "second"), model.slides.map { it.text })
        assertEquals(StoryBackground.Sunset, model.slides[0].background)
        assertEquals(StoryBackground.Night, model.slide.background)
    }

    @Test
    fun aStoryHasAtMostTenSlidesAndNeverLessThanOne() {
        val model = ComposerModel(true)
        repeat(9) { assertTrue(model.addSlide()) }
        assertEquals(StoryDraftStore.MAX_SLIDES, model.slides.size)
        assertFalse(model.addSlide())
        assertEquals(10, model.slides.size)
        assertEquals(9, model.current)
        assertEquals("every slide has its own key", 10, model.slides.map { it.key }.toSet().size)
        repeat(20) { model.removeCurrent() }
        assertEquals(1, model.slides.size)
        assertEquals(0, model.current)
    }

    @Test
    fun removingASlideKeepsAValidSlideSelectedAndLetsGoOfTheSelection() {
        val model = ComposerModel(true)
        model.addSlide()
        model.addSlide()
        model.select(1)
        model.change { it.copy(text = "middle") }
        model.selected = Selected.Sticker("anything")
        model.removeCurrent()
        assertEquals(listOf("", ""), model.slides.map { it.text })
        assertEquals(0, model.current)
        assertNull(model.selected)
        model.select(99)
        assertEquals(1, model.current)
        model.select(-4)
        assertEquals(0, model.current)
    }

    @Test
    fun aDraftIsLoadedBackIntoTheComposerAsItWasSaved() {
        val model = ComposerModel(true)
        val sticker = make(StickerKind.Place, StickerFields(label = "Rome"))!!
        val content = DraftContent(
            slides = listOf(
                DraftSlide(SlideMode.Text, "words", StoryBackground.Ocean, StoryFont.Modern, emptyList(), listOf(sticker), emptyList(), null),
                DraftSlide(SlideMode.Media, "", StoryBackground.Sunset, StoryFont.Classic, emptyList(), emptyList(), emptyList(), photo()),
            ),
            audience = StoryAudience.Custom,
            customIds = listOf(userId),
            allowReplies = false,
            ttlSeconds = 21_600,
        )
        model.load(content, "draft-1", listOf(null, null), allowedTtl = listOf(3_600, 21_600, 86_400))
        assertEquals(2, model.slides.size)
        assertEquals("draft-1", model.draftId)
        assertFalse("just loaded: nothing to save", model.dirty)
        assertEquals(StoryAudience.Custom, model.audience)
        assertEquals(setOf(userId), model.customIds)
        assertFalse(model.replies)
        assertEquals(21_600, model.ttl)
        assertEquals(listOf<StorySticker>(sticker), model.slides[0].stickers)
        assertEquals(StoryKind.Image, model.slides[1].media?.kind)
        assertEquals(0, model.current)
        // A lifetime the server doesn't offer any more falls back to the default; so does the default itself.
        model.load(content, "draft-1", emptyList(), allowedTtl = listOf(3_600, 86_400))
        assertNull(model.ttl)
        model.load(DraftContent(content.slides, content.audience, content.customIds, true, 86_400), "draft-1", emptyList(), allowedTtl = listOf(86_400, 21_600))
        assertNull(model.ttl)
    }

    @Test
    fun theDraftTheComposerSavesCarriesEverySlideAndChoice() {
        val model = ComposerModel(true)
        model.change { it.copy(text = "hello", overlays = emptyList()) }
        model.addSlide()
        model.change { SlideUi(it.key, SlideMode.Media, media = photo()) }
        model.audience = StoryAudience.CloseFriends
        model.replies = false
        val saved = model.toDraftContent()
        assertEquals(listOf(SlideMode.Text, SlideMode.Media), saved.slides.map { it.mode })
        assertEquals("hello", saved.slides[0].text)
        assertNull(saved.slides[0].media)
        assertEquals(StoryKind.Image, saved.slides[1].media?.kind)
        assertEquals(StoryAudience.CloseFriends, saved.audience)
        assertFalse(saved.allowReplies)
        assertEquals("no lifetime chosen means the day", 86_400, saved.ttlSeconds)
        model.ttl = 3_600
        assertEquals(3_600, model.toDraftContent().ttlSeconds)
    }

    @Test
    fun aSlideCountsAsWorkOnceItHasWordsAPictureAnOverlayOrASticker() {
        assertFalse(SlideUi(0).hasContent)
        assertFalse(SlideUi(0, text = "  ").hasContent)
        assertTrue(SlideUi(0, text = "x").hasContent)
        assertTrue(SlideUi(0, mode = SlideMode.Media, media = photo()).hasContent)
        assertTrue(SlideUi(0, stickers = listOf(make(StickerKind.Place, StickerFields(label = "Rome"))!!)).hasContent)
    }
}

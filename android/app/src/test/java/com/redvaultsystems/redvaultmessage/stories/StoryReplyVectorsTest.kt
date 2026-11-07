package com.redvaultsystems.redvaultmessage.stories

import com.redvaultsystems.redvaultmessage.data.model.ParsedBody
import com.redvaultsystems.redvaultmessage.data.model.RichMessage
import com.redvaultsystems.redvaultmessage.data.model.StoryReplyRef
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * A reply or reaction to a story is an ordinary chat message with a `storyReply` field. The vector is the body the web's own
 * serializeStoryReply made (resources/vectors/story_vectors.json, `storyReplyBody`).
 */
class StoryReplyVectorsTest {
    private val prefix = "" + 0.toChar() + "rv-msg:"
    private val web = vectors("story_vectors.json").str("storyReplyBody")
    private val ref = StoryReplyRef("00000000-0000-4000-8000-00000000000A", "5c1d0f7e-2b3a-4c8d-9e6f-a1b2c3d4e5f6", "image", "Photo", false)

    @Test
    fun theBodyMadeHereIsTheWebsByteForByte() {
        assertEquals(prefix + web, RichMessage.serializeStoryReply("Love this 😍", ref))
    }

    @Test
    fun theBodyTheWebMadeIsReadHere() {
        val parsed = RichMessage.parse(prefix + web) as ParsedBody.Text
        assertEquals("Love this 😍", parsed.text)
        val reply = parsed.storyReply!!
        assertEquals("00000000-0000-4000-8000-00000000000a", reply.authorId) // lower case
        assertEquals("5c1d0f7e-2b3a-4c8d-9e6f-a1b2c3d4e5f6", reply.storyId)
        assertEquals("image", reply.kind)
        assertEquals("Photo", reply.preview)
        assertEquals(false, reply.reaction)
    }

    @Test
    fun aReactionIsTheSameWithTheEmojiAsItsTextAndReactionTrue() {
        val body = RichMessage.serializeStoryReply("❤️", ref.copy(reaction = true))
        val parsed = RichMessage.parse(body) as ParsedBody.Text
        assertEquals(true, parsed.storyReply!!.reaction)
        assertEquals("❤️", parsed.text)
    }

    @Test
    fun theLabelIsTidiedAndCappedAtEightyCharacters() {
        val long = RichMessage.serializeStoryReply("hi", ref.copy(preview = "  a   b  " + "x".repeat(200)))
        val reply = (RichMessage.parse(long) as ParsedBody.Text).storyReply!!
        assertEquals(80, reply.preview.length)
        assertTrue(reply.preview.startsWith("a b xxx"))
    }

    @Test
    fun aStoryReplyThatIsMalformedMakesTheWholeMessageInvalidAsOnTheWeb() {
        fun body(storyReply: String) = prefix + "{\"type\":\"text\",\"version\":1,\"text\":\"hi\",\"storyReply\":" + storyReply + "}"
        val good = "{\"authorId\":\"00000000-0000-4000-8000-00000000000a\",\"storyId\":\"5c1d0f7e-2b3a-4c8d-9e6f-a1b2c3d4e5f6\",\"kind\":\"video\",\"preview\":\"Video\",\"reaction\":false}"
        assertTrue(RichMessage.parse(body(good)) is ParsedBody.Text)
        for (bad in listOf(
            good.replace("\"video\"", "\"audio\""),
            good.replace("\"video\"", "\"reel\""),
            good.replace("00000000-0000-4000-8000-00000000000a", "not-a-uuid"),
            good.replace("5c1d0f7e-2b3a-4c8d-9e6f-a1b2c3d4e5f6", "5c1d0f7e"),
            good.replace("\"reaction\":false", "\"reaction\":\"no\""),
            good.replace(",\"reaction\":false", ""),
            good.replace("\"preview\":\"Video\"", "\"preview\":5"),
            "null",
            "[]",
            "\"x\"",
        )) {
            assertEquals(bad, ParsedBody.Invalid, RichMessage.parse(body(bad)))
        }
    }
}

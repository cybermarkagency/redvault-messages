package com.redvaultsystems.redvaultmessage.stories

import com.redvaultsystems.redvaultmessage.data.api.StoryAvailability
import com.redvaultsystems.redvaultmessage.data.api.StoryConfigParser
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** What `GET /api/stories/config` decides: whether this app shows Stories at all (docs/stories-advanced/api.md). */
class StoryConfigTest {
    private fun parse(json: String) = StoryConfigParser.parse(Json.parseToJsonElement(json))
    private fun on(json: String) = parse(json) as StoryAvailability.On

    @Test
    fun enabledFalseIsOff() {
        assertEquals(StoryAvailability.Off, parse("""{"enabled":false,"media":true,"audio":true,"interactive":true,"ttlSeconds":[86400]}"""))
        assertEquals(StoryAvailability.Off, parse("""{"enabled":false}"""))
    }

    @Test
    fun aMissingEnabledMeansOn() {
        // A server from before the switch says nothing about it.
        val config = on("""{"media":true,"interactive":true,"ttlSeconds":[3600,86400]}""")
        assertTrue(config.media)
        assertTrue(config.interactive)
        assertEquals(listOf(3600, 86400), config.ttlSeconds)
        assertTrue(parse("""{"enabled":null,"media":false}""") is StoryAvailability.On)
        assertTrue(parse("""{"enabled":true,"media":false}""") is StoryAvailability.On)
        assertTrue(parse("{}") is StoryAvailability.On)
    }

    @Test
    fun anEnabledThatIsNeitherTrueNorFalseCannotBeTrustedAndIsOff() {
        for (bad in listOf("\"false\"", "\"true\"", "0", "1", "[]", "{}", "\"\"")) {
            assertEquals("enabled=$bad", StoryAvailability.Off, parse("""{"enabled":$bad,"media":true}"""))
        }
    }

    @Test
    fun aReplyThatIsNotAnObjectIsOff() {
        for (bad in listOf("null", "[]", "true", "\"on\"", "3")) assertEquals(bad, StoryAvailability.Off, parse(bad))
    }

    @Test
    fun theOtherSwitchesAreOffUnlessTheyAreTrue() {
        val config = on("""{"enabled":true,"media":"yes","interactive":1}""")
        assertFalse(config.media)
        assertFalse(config.interactive)
        val missing = on("""{"enabled":true}""")
        assertFalse(missing.media)
        assertFalse(missing.interactive)
    }

    @Test
    fun theLifetimesAreOnlyTheOnesThatMakeSense() {
        assertEquals(listOf(86_400), on("""{"enabled":true}""").ttlSeconds)
        assertEquals(listOf(86_400), on("""{"enabled":true,"ttlSeconds":[]}""").ttlSeconds)
        assertEquals(listOf(86_400), on("""{"enabled":true,"ttlSeconds":"3600"}""").ttlSeconds)
        assertEquals(listOf(3600, 21600), on("""{"enabled":true,"ttlSeconds":[3600,"x",-5,0,1.5,21600,null,99999999999]}""").ttlSeconds)
    }
}

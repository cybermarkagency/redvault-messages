package com.redvaultsystems.redvaultmessage.identity

import kotlin.random.Random

/** Port of ui/src/lib/identity.ts: suggests a handle locally. Nothing is sent anywhere. */
object HandleGenerator {
    private val ADJECTIVES = listOf(
        "quiet", "still", "hollow", "amber", "cinder", "pale", "drift", "ember", "frost", "dusky",
        "salt", "brave", "lone", "hidden", "faint", "slow", "deep", "clear", "soft", "far",
    )

    private val NOUNS = listOf(
        "harbor", "grove", "ridge", "hollow", "meadow", "summit", "brook", "thicket", "ferry", "lantern",
        "orchard", "quarry", "hearth", "tundra", "glacier", "valley", "bramble", "willow", "canyon", "marsh",
    )

    private const val HEX = "0123456789abcdef"

    fun generate(random: Random = Random.Default): String {
        val suffix = (1..4).map { HEX[random.nextInt(HEX.length)] }.joinToString("")
        return "${ADJECTIVES.random(random)}-${NOUNS.random(random)}·$suffix"
    }
}

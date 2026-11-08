// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

/**
 * 256 short, distinct, easy-to-say words, one per byte: the same list as
 * ui/src/lib/sas.ts and the desktop app, for call safety words and the
 * password generator's word mode. Never reorder it: both uses depend on it.
 */
object WordList {
    val words: List<String> = listOf(
        "acorn", "adobe", "agent", "alpine", "amber", "anchor", "angle", "apple", "arbor", "arctic", "arrow", "aspen", "atlas",
        "attic", "autumn", "avenue", "badge", "bakery", "bamboo", "banjo", "barley", "basin", "beacon", "bishop", "blanket",
        "bloom", "bonfire", "border", "breeze", "bridge", "bronze", "bubble", "cabin", "cactus", "camel", "canal", "candle",
        "canyon", "carbon", "cargo", "castle", "cedar", "cellar", "chalk", "cherry", "chimney", "cider", "cinder", "citrus",
        "clover", "cobalt", "cocoa", "comet", "coral", "cotton", "cradle", "crater", "crystal", "cypress", "dahlia", "delta",
        "desert", "dolphin", "domino", "dragon", "drift", "dune", "eagle", "echo", "eclipse", "elbow", "ember", "emerald",
        "engine", "falcon", "fable", "feather", "fennel", "ferry", "fiddle", "fjord", "flint", "forest", "fossil", "fountain",
        "galaxy", "garden", "garnet", "gazelle", "geyser", "ginger", "glacier", "globe", "granite", "gravel", "grove", "hammock",
        "harbor", "harvest", "hazel", "helmet", "heron", "hollow", "honey", "horizon", "indigo", "iris", "island", "ivory",
        "jacket", "jasmine", "jungle", "juniper", "kayak", "kettle", "kiwi", "koala", "lagoon", "lantern", "lava", "lemon",
        "lilac", "linen", "lotus", "lunar", "magnet", "mango", "maple", "marble", "meadow", "melon", "meteor", "mirror",
        "mosaic", "moss", "nectar", "needle", "nimbus", "nomad", "nutmeg", "oasis", "ocean", "olive", "onyx", "opal", "orbit",
        "orchid", "otter", "oyster", "paddle", "panda", "papaya", "parrot", "pebble", "pepper", "piano", "pillow", "pine",
        "planet", "plum", "polar", "poppy", "prism", "pumpkin", "quartz", "quill", "quiver", "rabbit", "radar", "raven", "reef",
        "ribbon", "river", "rocket", "saddle", "saffron", "salmon", "sapphire", "satin", "shadow", "shell", "sierra", "silver",
        "sketch", "sparrow", "spruce", "summit", "sunset", "swan", "tango", "temple", "thistle", "thunder", "tiger", "timber",
        "topaz", "tulip", "tundra", "turtle", "valley", "vapor", "velvet", "violet", "walnut", "willow", "window", "winter",
        "wizard", "yarrow", "zebra", "zephyr", "zinnia", "anvil", "ballad", "bramble", "cobble", "dusk", "fern", "gable", "hive",
        "jade", "kelp", "larch", "mint", "nova", "oak", "pearl", "quail", "rain", "sage", "tide", "basalt", "beetle", "blossom",
        "bugle", "cabana", "caramel", "chestnut", "cobra", "condor", "crocus", "dingo", "falafel", "firefly", "gecko", "goblet",
        "hamlet", "igloo", "jigsaw", "kestrel", "lichen", "lobster", "mesa", "muffin", "narwhal", "nugget",
    )

    init {
        check(words.size == 256) { "The word list must have 256 words" }
    }
}

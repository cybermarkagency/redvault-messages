// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Short authentication words for a call ("harbor velvet orbit cinder").
// Derived from both sides' DTLS certificate fingerprints, which is what the
// encrypted media is actually pinned to: if anyone were relaying the media
// with their own keys, the two people would see different words. Saying
// them aloud checks the live connection itself, on top of the sealed setup.
import sodium from "libsodium-wrappers-sumo";

// 256 short, distinct, easy-to-say words: one per byte. The same list as the
// desktop's (desktop/core/src/call_crypto.cpp), which the password generator's
// word mode also draws from.
export const WORDS = (
  "acorn adobe agent alpine amber anchor angle apple arbor arctic arrow aspen atlas attic autumn avenue badge " +
  "bakery bamboo banjo barley basin beacon bishop blanket bloom bonfire border breeze bridge bronze bubble " +
  "cabin cactus camel canal candle canyon carbon cargo castle cedar cellar chalk cherry chimney cider cinder " +
  "citrus clover cobalt cocoa comet coral cotton cradle crater crystal cypress dahlia delta desert dolphin " +
  "domino dragon drift dune eagle echo eclipse elbow ember emerald engine falcon fable feather fennel ferry " +
  "fiddle fjord flint forest fossil fountain galaxy garden garnet gazelle geyser ginger glacier globe granite " +
  "gravel grove hammock harbor harvest hazel helmet heron hollow honey horizon indigo iris island ivory jacket " +
  "jasmine jungle juniper kayak kettle kiwi koala lagoon lantern lava lemon lilac linen lotus lunar magnet " +
  "mango maple marble meadow melon meteor mirror mosaic moss nectar needle nimbus nomad nutmeg oasis ocean " +
  "olive onyx opal orbit orchid otter oyster paddle panda papaya parrot pebble pepper piano pillow pine planet " +
  "plum polar poppy prism pumpkin quartz quill quiver rabbit radar raven reef ribbon river rocket saddle " +
  "saffron salmon sapphire satin shadow shell sierra silver sketch sparrow spruce summit sunset swan tango " +
  "temple thistle thunder tiger timber topaz tulip tundra turtle valley vapor velvet violet walnut willow " +
  "window winter wizard yarrow zebra zephyr zinnia anvil ballad bramble cobble dusk fern gable hive jade " +
  "kelp larch mint nova oak pearl quail rain sage tide basalt beetle blossom bugle cabana caramel chestnut " +
  "cobra condor crocus dingo falafel firefly gecko goblet hamlet igloo jigsaw kestrel lichen lobster mesa " +
  "muffin narwhal nugget"
).split(" ");

export function sasWords(fingerprintA: string, fingerprintB: string, count = 4): string[] {
  const [first, second] = [fingerprintA.toUpperCase(), fingerprintB.toUpperCase()].sort();
  // BLAKE2b's shortest output is 16 bytes; each word takes one.
  const digest: Uint8Array = sodium.crypto_generichash(16, `redvault-call-sas-v1|${first}|${second}`, null);
  return Array.from(digest.subarray(0, count), (byte: number) => WORDS[byte]);
}

/** The DTLS fingerprint in an SDP ("sha-256 AB:CD:…"), or null. */
export function sdpFingerprint(sdp: string | undefined | null): string | null {
  const match = /^a=fingerprint:(\S+ [0-9A-Fa-f:]+)/m.exec(sdp ?? "");
  return match ? match[1] : null;
}

export const SAS_WORD_COUNT = new Set(WORDS).size;

/**
 * Safety words for a group call. There's no end-to-end DTLS connection to
 * pin in a call through an SFU; what the media is pinned to instead is the
 * identity key each participant's room key was sealed to. So the words come
 * from the call and every participant's identity key as this app knows it:
 * if the server had swapped anyone's key (to receive the room key itself),
 * that person's screen would show different words from everyone else's.
 */
export function groupSasWords(callId: string, participants: { userId: string; publicKey: Uint8Array }[], count = 4): string[] {
  const entries = participants.map((p) => `${p.userId}:${sodium.to_hex(p.publicKey)}`).sort();
  const digest: Uint8Array = sodium.crypto_generichash(16, `redvault-group-call-sas-v1|${callId}|${entries.join("|")}`, null);
  return Array.from(digest.subarray(0, count), (byte: number) => WORDS[byte]);
}

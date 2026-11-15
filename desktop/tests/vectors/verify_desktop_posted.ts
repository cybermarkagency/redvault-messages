// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The other half of the desktop <-> web check for the story studio: opens the stories the DESKTOP code made with the
// WEB app's own code (ui/src/lib on branch paid-communities, where the web studio lives). The desktop test
// tests/studio_app_test.cpp ("stories the desktop studio makes are written ...") writes
// <desktop build>/tests/story-desktop-out/desktop_posted.json. Not part of any build: copy this file into ui/src/test/
// as verifyDesktopPosted.test.ts, set DESKTOP_POSTED to that JSON file, run
//
//   cd ui && DESKTOP_POSTED=/path/to/desktop_posted.json npx vitest run src/test/verifyDesktopPosted.test.ts
//
// and delete the copy. Android's StoryMetaParser is a line-by-line port of the same parseStoryMeta, pinned to the same
// web vectors; it was not run against this file.
import { readFileSync } from "node:fs";
import { expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import { decryptFile } from "../lib/fileCrypto.js";
import { decodeStory, storyPreview } from "../lib/stories.js";
import type { StoryDto } from "../lib/storiesApi.js";
import { captionAt } from "../lib/storyCrypto.js";

/** jsdom's Blob has no arrayBuffer(); FileReader works everywhere. */
function readBlob(blob: Blob): Promise<Uint8Array> {
  return new Promise((resolveBlob, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolveBlob(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error);
    reader.readAsArrayBuffer(blob);
  });
}

it("the web app opens every story the desktop studio made, as the desktop made it", async () => {
  await crypto.ready();
  const data = JSON.parse(readFileSync(process.env.DESKTOP_POSTED as string, "utf8"));
  const alice = crypto.fromBase64(data.alice.publicKey);
  const bob = crypto.fromBase64(data.bob.privateKey);
  const by = (name: string) => data.stories.find((s: { name: string }) => s.name === name);
  const open = (s: Record<string, unknown>) => {
    const dto: StoryDto = {
      id: s.id as string,
      authorId: "00000000-0000-4000-8000-0000000000a1",
      kind: s.kind as StoryDto["kind"],
      metaCiphertext: s.metaCiphertext as string,
      metaNonce: s.metaNonce as string,
      mediaSize: s.mediaSize as number | null,
      hasMedia: s.hasMedia as boolean,
      allowReplies: true,
      createdAt: "2026-11-15T12:00:00.000Z",
      expiresAt: "2026-11-16T12:00:00.000Z",
      wrappedKey: s.wrappedKeyForBob as string,
    };
    return decodeStory(dto, alice, bob);
  };

  for (const s of data.stories) {
    const story = open(s);
    expect(story, s.name).not.toBeNull();
    // What the web reads is what the desktop wrote, field for field (nothing dropped, nothing added).
    expect(story!.meta, s.name).toEqual(s.expected);
    if (s.hasMedia) {
      const media = story!.meta.media!;
      expect(media.chunkSize).toBe(65536);
      expect(media.encryptedSize).toBe(s.mediaSize);
      const plain = await readBlob(
        await decryptFile([crypto.fromBase64(s.media)], { key: crypto.fromBase64(media.key), header: crypto.fromBase64(media.header), chunkSize: media.chunkSize, encryptedSize: media.encryptedSize, ciphertextSha256: media.sha256 }, media.mime)
      );
      expect(plain.length, s.name).toBe(s.plainLength);
      expect(crypto.toBase64(plain), s.name).toBe(s.plain);
    }
  }

  // The voice story: a WAV with its bars, title, look, subtitles and a pulsing word.
  const voice = open(by("voice"))!;
  expect(voice.kind).toBe("audio");
  expect(voice.meta.media?.mime).toBe("audio/wav");
  expect(voice.meta.media?.width).toBe(1);
  expect(voice.meta.waveform).toHaveLength(48);
  expect(voice.meta.text).toBe("Desktop voice");
  expect(voice.meta.background).toBe("ocean");
  expect(captionAt(voice.meta.captions ?? [], 10)).toBe("first line");
  expect(voice.meta.overlays[0].anim).toBe("pulse");
  expect(storyPreview(voice)).toBe("Desktop voice");
  // The edited photo and the collage are JPEGs made here.
  for (const name of ["photo", "collage"]) {
    const s = by(name);
    const bytes = crypto.fromBase64(s.plain);
    expect([bytes[0], bytes[1], bytes[2]], name).toEqual([0xff, 0xd8, 0xff]);
  }
  expect(open(by("collage"))!.meta.media).toMatchObject({ width: 1080, height: 1920, mime: "image/jpeg" });
  // The video's subtitles were read from an .srt (markup dropped).
  expect(open(by("video"))!.meta.captions?.map((c) => c.text)).toEqual(["Hello there", "from the desktop"]);
  // The template: its sticker read by the web's sticker parser, the emoji bouncing.
  const template = open(by("template"))!;
  expect(template.meta.text).toBe("Coming soon");
  expect(template.meta.stickers?.[0]).toMatchObject({ type: "countdown", title: "The big day", endsAt: 1_900_000_000_000 + 7 * 86_400_000 });
  expect(template.meta.overlays[0]).toMatchObject({ type: "emoji", anim: "bounce" });
});

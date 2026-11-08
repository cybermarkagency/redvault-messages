// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Produces story_draft_v2_vectors.json with the web app's own code (lib/storyDrafts.ts, format version 2: several slides), so the
// Android app's sealed drafts use the same key derivation, binding and layout. Drafts never leave a device, so nothing crosses between
// apps; this only pins that the stores mean the same thing. Not part of any build: copy it into ui/src/test/ as
// makeAndroidDraftVectors.test.ts, run
//
//   cd ui && npx vitest run src/test/makeAndroidDraftVectors.test.ts
//
// and delete the copy. The output lands next to this file (android/app/src/test/resources/vectors/).
import "fake-indexeddb/auto";
import { writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import { listDrafts, loadDraft, saveDraft, type DraftContent, type DraftSlide } from "../lib/storyDrafts.js";
import type { StoryIdentity } from "../lib/stories.js";
import type { LinkSticker, PollSticker } from "../lib/storyCrypto.js";

const OUT = resolve(process.cwd(), "../android/app/src/test/resources/vectors/story_draft_v2_vectors.json");

function readBytes(blob: Blob): Promise<Uint8Array> {
  return new Promise((resolveBlob, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolveBlob(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error);
    reader.readAsArrayBuffer(blob);
  });
}

function rawRows(): Promise<{ id: string; userId: string; updatedAt: number; nonce: Uint8Array; sealed: Uint8Array; media: (ArrayBuffer | null)[] }[]> {
  return new Promise((resolveRows, reject) => {
    const open = indexedDB.open("redvault-story-drafts", 1);
    open.onsuccess = () => {
      const db = open.result;
      const req = db.transaction("drafts").objectStore("drafts").getAll();
      req.onsuccess = () => {
        db.close();
        resolveRows(req.result);
      };
      req.onerror = () => reject(req.error);
    };
    open.onerror = () => reject(open.error);
  });
}

it("writes the vectors", async () => {
  const sodium = await crypto.ready();
  const b64 = crypto.toBase64;
  const kp = sodium.crypto_box_seed_keypair(new Uint8Array(32).fill(21));
  const me: StoryIdentity = { userId: "00000000-0000-4000-8000-0000000000a1", publicKey: kp.publicKey, privateKey: kp.privateKey };

  const poll: PollSticker = { id: "pollsticker1", type: "poll", x: 0.4, y: 0.6, scale: 1.2, rotation: 5, question: "Secret plan? 🎉", options: ["Yes", "No"], results: "everyone", closesAt: 1_900_000_000_000 };
  const link: LinkSticker = { id: "linksticker1", type: "link", x: 0.5, y: 0.85, scale: 1, rotation: 0, url: "https://example.com/party?x=1", label: "Details" };
  const photoBytes = new Uint8Array(150_000).map((_, i) => (i * 7 + 3) % 251);
  const videoBytes = new Uint8Array(70_001).map((_, i) => (i * 13 + 5) % 253);
  const textSlide: DraftSlide = {
    mode: "text",
    text: "my unfinished thought about the surprise party 🎈",
    background: "ocean",
    font: "typewriter",
    overlays: [
      { type: "text", text: "Shh", x: 0.3, y: 0.2, scale: 1.4, rotation: 10, color: "yellow", boxed: true, font: "strong", anim: "pulse" },
      { type: "emoji", text: "🤫", x: 0.7, y: 0.25, scale: 1.1, rotation: 0, color: "white", boxed: false, font: "classic" },
    ],
    stickers: [poll, link],
    captions: [],
    media: null,
  };
  const photoSlide: DraftSlide = { mode: "media", text: "", background: "sunset", font: "classic", overlays: [], stickers: [], captions: [], media: { kind: "image", blob: new Blob([photoBytes], { type: "image/jpeg" }), mime: "image/jpeg", width: 800, height: 1200 } };
  const videoSlide: DraftSlide = {
    mode: "media",
    text: "",
    background: "sunset",
    font: "classic",
    overlays: [],
    stickers: [],
    captions: [{ startMs: 0, endMs: 1500, text: "first" }],
    media: { kind: "video", blob: new Blob([videoBytes], { type: "video/mp4" }), mime: "video/mp4", width: 1080, height: 1920, durationMs: 4200 },
  };
  const contents: [string, DraftContent, Uint8Array[]][] = [
    ["three slides: text, photo, video", { slides: [textSlide, photoSlide, videoSlide], audience: "custom", customIds: ["00000000-0000-4000-8000-00000000000c", "00000000-0000-4000-8000-00000000000d"], allowReplies: false, ttlSeconds: 6 * 3600 }, [new Uint8Array(0), photoBytes, videoBytes]],
    ["one text slide", { slides: [{ ...textSlide, stickers: [], overlays: [] }], audience: "close_friends", customIds: [], allowReplies: true, ttlSeconds: 86400 }, [new Uint8Array(0)]],
  ];

  const drafts: unknown[] = [];
  for (const [name, content, plains] of contents) {
    const id = await saveDraft(me, content);
    const row = (await rawRows()).find((r) => r.id === id)!;
    const summary = (await listDrafts(me)).find((d) => d.id === id)!;
    const back = (await loadDraft(me, id))!;
    expect(back).not.toBeNull();
    const slides = [];
    for (let i = 0; i < back.slides.length; i++) {
      const { media, ...rest } = back.slides[i];
      if (media) expect(await readBytes(media.blob)).toEqual(plains[i]);
      slides.push({ ...rest, media: media ? { kind: media.kind, mime: media.mime, width: media.width, height: media.height, ...(media.durationMs ? { durationMs: media.durationMs } : {}), bytes: b64(plains[i]) } : null });
    }
    const { slides: _s, ...story } = back;
    void _s;
    drafts.push({
      name,
      id,
      userId: row.userId,
      updatedAt: row.updatedAt,
      nonce: b64(Uint8Array.from(row.nonce)),
      sealed: b64(Uint8Array.from(row.sealed)),
      media: row.media.map((m) => (m ? b64(new Uint8Array(m)) : null)),
      summary: { slideCount: summary.slideCount, preview: summary.preview, stickerCount: summary.stickerCount },
      // What the web app's loadDraft hands back (each photo or video as bytes).
      expected: { ...story, slides },
    });
  }

  writeFileSync(
    OUT,
    JSON.stringify(
      {
        note: "Made by android/app/src/test/resources/vectors/make_android_draft_vectors.ts with the web app's own code.",
        userId: me.userId,
        publicKey: b64(me.publicKey),
        privateKey: b64(me.privateKey),
        otherUserId: "00000000-0000-4000-8000-0000000000a2",
        otherPrivateKey: b64(sodium.crypto_box_seed_keypair(new Uint8Array(32).fill(22)).privateKey),
        drafts,
      },
      null,
      2
    ) + "\n"
  );
});

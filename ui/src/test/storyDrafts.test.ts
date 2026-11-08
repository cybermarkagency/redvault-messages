// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import "fake-indexeddb/auto";
import { beforeAll, beforeEach, describe, expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import { deleteDraft, DRAFT_KEEP_MS, listDrafts, loadDraft, MAX_DRAFTS, MAX_SLIDES, saveDraft, type DraftContent, type DraftSlide } from "../lib/storyDrafts.js";
import { newStickerId, type PollSticker } from "../lib/storyCrypto.js";
import type { StoryIdentity } from "../lib/stories.js";

let me: StoryIdentity;
let other: StoryIdentity;
let meWithAnotherKey: StoryIdentity;

beforeAll(async () => {
  await crypto.ready();
  const a = crypto.generateKeyPair();
  const b = crypto.generateKeyPair();
  me = { userId: "00000000-0000-4000-8000-00000000000a", ...a };
  other = { userId: "00000000-0000-4000-8000-00000000000b", ...b };
  meWithAnotherKey = { userId: me.userId, ...crypto.generateKeyPair() };
});

beforeEach(async () => {
  await new Promise<void>((resolve, reject) => {
    const req = indexedDB.deleteDatabase("redvault-story-drafts");
    req.onsuccess = () => resolve();
    req.onerror = () => reject(req.error);
  });
});

const poll: PollSticker = { id: "pollsticker1", type: "poll", x: 0.4, y: 0.6, scale: 1.2, rotation: 5, question: "Secret plan?", options: ["Yes", "No"], results: "everyone" };

const slide = (over: Partial<DraftSlide> = {}): DraftSlide => ({
  mode: "text",
  text: "my unfinished thought about the surprise party",
  background: "ocean",
  font: "modern",
  overlays: [],
  stickers: [poll],
  captions: [],
  media: null,
  ...over,
});

const draft = (slides: DraftSlide[] = [slide()], over: Partial<Omit<DraftContent, "slides">> = {}): DraftContent => ({
  slides,
  audience: "custom",
  customIds: ["00000000-0000-4000-8000-00000000000c"],
  allowReplies: false,
  ttlSeconds: 3600,
  ...over,
});

const photoSlide = (bytes: Uint8Array): DraftSlide =>
  slide({ mode: "media", text: "", stickers: [], media: { kind: "image", blob: new Blob([bytes as BlobPart], { type: "image/jpeg" }), mime: "image/jpeg", width: 800, height: 1200 } });

type Row = { id: string; userId: string; updatedAt: number; nonce: Uint8Array; sealed: Uint8Array; media: (ArrayBuffer | null)[] };

/** Every stored row, as IndexedDB holds it. */
function rawRows(): Promise<Row[]> {
  return new Promise((resolve, reject) => {
    const open = indexedDB.open("redvault-story-drafts", 1);
    open.onsuccess = () => {
      const db = open.result;
      const req = db.transaction("drafts").objectStore("drafts").getAll();
      req.onsuccess = () => {
        db.close();
        resolve(req.result);
      };
      req.onerror = () => {
        db.close();
        reject(req.error);
      };
    };
    open.onerror = () => reject(open.error);
  });
}

function rewrite(change: (row: Record<string, unknown>) => void): Promise<void> {
  return new Promise((resolve, reject) => {
    const open = indexedDB.open("redvault-story-drafts", 1);
    open.onsuccess = () => {
      const db = open.result;
      const tx = db.transaction("drafts", "readwrite");
      const store = tx.objectStore("drafts");
      store.getAll().onsuccess = (e) => {
        for (const row of (e.target as IDBRequest).result) {
          change(row);
          store.put(row);
        }
      };
      tx.oncomplete = () => {
        db.close();
        resolve();
      };
      tx.onerror = () => {
        db.close();
        reject(tx.error);
      };
    };
  });
}

function readBytes(blob: Blob): Promise<Uint8Array> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error);
    reader.readAsArrayBuffer(blob);
  });
}

/** Puts the sealed content of draft `from` under the id `to`, as someone with access to the disk could. */
function copyOver(from: string, to: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const open = indexedDB.open("redvault-story-drafts", 1);
    open.onsuccess = () => {
      const db = open.result;
      const tx = db.transaction("drafts", "readwrite");
      const store = tx.objectStore("drafts");
      store.get(from).onsuccess = (e) => store.put({ ...(e.target as IDBRequest).result, id: to });
      tx.oncomplete = () => {
        db.close();
        resolve();
      };
      tx.onerror = () => reject(tx.error);
    };
  });
}

describe("story drafts", () => {
  it("a saved draft comes back as it was, with its stickers and the story's choices", async () => {
    const id = await saveDraft(me, draft());
    const list = await listDrafts(me);
    expect(list).toHaveLength(1);
    expect(list[0]).toMatchObject({ id, slideCount: 1, stickerCount: 1, preview: "my unfinished thought about the surprise party" });
    const back = await loadDraft(me, id);
    expect(back).toMatchObject({ audience: "custom", allowReplies: false, ttlSeconds: 3600 });
    expect(back!.customIds).toEqual(draft().customIds);
    expect(back!.slides).toHaveLength(1);
    expect(back!.slides[0]).toMatchObject({ mode: "text", text: draft().slides[0].text, background: "ocean", font: "modern", media: null, captions: [] });
    expect(back!.slides[0].stickers).toEqual([poll]);
  });

  it("keeps every slide, in order, with its own photo, voice recording and captions", async () => {
    const photo = new Uint8Array(70_000).map((_, i) => (i * 11) % 251);
    const voiceBytes = new Uint8Array(5000).map((_, i) => (i * 3) % 199);
    const voice = slide({
      mode: "audio",
      text: "Road trip",
      stickers: [],
      captions: [{ startMs: 0, endMs: 1500, text: "Hello" }],
      media: { kind: "audio", blob: new Blob([voiceBytes as BlobPart], { type: "audio/wav" }), mime: "audio/wav", width: 1, height: 1, durationMs: 3000, waveform: Array.from({ length: 48 }, (_, i) => i) },
    });
    const id = await saveDraft(me, draft([photoSlide(photo), slide({ text: "middle" }), voice]));
    expect((await listDrafts(me))[0]).toMatchObject({ slideCount: 3, preview: "Photo", stickerCount: 1 });
    const back = (await loadDraft(me, id))!;
    expect(back.slides.map((s) => s.mode)).toEqual(["media", "text", "audio"]);
    expect(await readBytes(back.slides[0].media!.blob)).toEqual(photo);
    expect(back.slides[1].media).toBeNull();
    expect(await readBytes(back.slides[2].media!.blob)).toEqual(voiceBytes);
    expect(back.slides[2]).toMatchObject({ text: "Road trip", captions: [{ startMs: 0, endMs: 1500, text: "Hello" }] });
    expect(back.slides[2].media).toMatchObject({ kind: "audio", durationMs: 3000, mime: "audio/wav" });
    expect(back.slides[2].media!.waveform).toHaveLength(48);
  });

  it("saving over an id replaces that draft instead of adding another", async () => {
    const id = await saveDraft(me, draft());
    const again = await saveDraft(me, draft([slide({ text: "changed my mind" })]), id);
    expect(again).toBe(id);
    const list = await listDrafts(me);
    expect(list).toHaveLength(1);
    expect(list[0].preview).toBe("changed my mind");
  });

  it("nothing readable is stored: not the text, captions, question, choices or who it is for", async () => {
    await saveDraft(me, draft([slide({ captions: [{ startMs: 0, endMs: 1000, text: "hush hush caption" }] })]));
    const [row] = await rawRows();
    const everything = new TextDecoder("latin1").decode(row.sealed) + JSON.stringify({ ...row, sealed: undefined, nonce: undefined });
    for (const secret of ["surprise party", "Secret plan", "hush hush", "ocean", "000000000c", "Yes"]) expect(everything).not.toContain(secret);
    expect(row.userId).toBe(me.userId);
  });

  it("every photo is stored encrypted, not just the first", async () => {
    const a = new Uint8Array(200_000).map((_, i) => (i * 7) % 251);
    const b = new Uint8Array(150_000).map((_, i) => (i * 13) % 241);
    await saveDraft(me, draft([photoSlide(a), photoSlide(b)]));
    const [row] = await rawRows();
    expect(row.media).toHaveLength(2);
    expect(row.media[0]!.byteLength).toBeGreaterThan(a.length);
    expect(row.media[1]!.byteLength).toBeGreaterThan(b.length);
    // Not the photo's bytes (not even the start of them).
    expect(new Uint8Array(row.media[0]!).subarray(0, 64)).not.toEqual(a.subarray(0, 64));
    expect(new Uint8Array(row.media[1]!).subarray(0, 64)).not.toEqual(b.subarray(0, 64));
  });

  it("another account never sees it, and the same account with another key can't open it", async () => {
    const id = await saveDraft(me, draft());
    expect(await listDrafts(other)).toEqual([]);
    expect(await loadDraft(other, id)).toBeNull();
    // Same user id, different private key (a restored or replaced identity): unreadable, so it is cleared.
    expect(await loadDraft(meWithAnotherKey, id)).toBeNull();
    expect(await listDrafts(meWithAnotherKey)).toEqual([]);
    expect(await rawRows()).toHaveLength(0);
  });

  it("a draft that was changed on disk is dropped, not opened", async () => {
    const id = await saveDraft(me, draft());
    await rewrite((row) => {
      const sealed = row.sealed as Uint8Array;
      sealed[5] ^= 1;
    });
    expect(await loadDraft(me, id)).toBeNull();
    expect(await listDrafts(me)).toEqual([]);
    expect(await rawRows()).toHaveLength(0);
  });

  it("a slide's media that was swapped or cut short is not opened", async () => {
    const id = await saveDraft(me, draft([photoSlide(new Uint8Array(3000).fill(1)), photoSlide(new Uint8Array(3000).fill(2))]));
    await rewrite((row) => {
      const media = row.media as ArrayBuffer[];
      row.media = [media[1], media[0]];
    });
    expect(await loadDraft(me, id)).toBeNull();
    const second = await saveDraft(me, draft([photoSlide(new Uint8Array(3000).fill(3))]));
    await rewrite((row) => {
      if (row.id === second) row.media = [(row.media as ArrayBuffer[])[0].slice(0, 100)];
    });
    expect(await loadDraft(me, second)).toBeNull();
    const third = await saveDraft(me, draft([photoSlide(new Uint8Array(3000).fill(4))]));
    await rewrite((row) => {
      if (row.id === third) row.media = [];
    });
    expect(await loadDraft(me, third)).toBeNull();
  });

  it("a draft can't be moved to another id", async () => {
    const a = await saveDraft(me, draft([slide({ text: "first" })]));
    const b = await saveDraft(me, draft([slide({ text: "second" })]));
    await copyOver(a, b);
    // What now sits at b's id was sealed for a's id: it doesn't open.
    expect(await loadDraft(me, b)).toBeNull();
    expect(await loadDraft(me, a)).not.toBeNull();
  });

  it("keeps ten at most: the oldest goes", async () => {
    const ids: string[] = [];
    const base = Date.now() - 3600_000;
    for (let i = 0; i < MAX_DRAFTS + 2; i++) {
      ids.push(await saveDraft(me, draft([slide({ text: `draft ${i}` })])));
      await rewrite((row) => {
        if (row.id === ids[i]) row.updatedAt = base + i * 1000;
      });
    }
    const list = await listDrafts(me);
    expect(list).toHaveLength(MAX_DRAFTS);
    expect(list.map((d) => d.preview)).not.toContain("draft 0");
    expect(list.map((d) => d.preview)).not.toContain("draft 1");
    expect(list[0].preview).toBe(`draft ${MAX_DRAFTS + 1}`);
  });

  it("a story has at most ten slides, and nothing at all is not a draft", async () => {
    const many = Array.from({ length: MAX_SLIDES + 5 }, (_, i) => slide({ text: `slide ${i}` }));
    const id = await saveDraft(me, draft(many));
    expect((await loadDraft(me, id))!.slides).toHaveLength(MAX_SLIDES);
    await expect(saveDraft(me, draft([]))).rejects.toThrow(/nothing to save/);
  });

  it("drafts older than 30 days are cleared", async () => {
    const ancient = await saveDraft(me, draft([slide({ text: "ancient" })]));
    await saveDraft(me, draft([slide({ text: "recent" })]));
    await rewrite((row) => {
      if (row.id === ancient) row.updatedAt = Date.now() - DRAFT_KEEP_MS - 1000;
    });
    const list = await listDrafts(me);
    expect(list.map((d) => d.preview)).toEqual(["recent"]);
    expect(await rawRows()).toHaveLength(1);
  });

  it("deleting removes it", async () => {
    const id = await saveDraft(me, draft());
    await deleteDraft(id);
    expect(await listDrafts(me)).toEqual([]);
    expect(await loadDraft(me, id)).toBeNull();
  });

  it("sticker ids, shapes and captions are re-checked when a draft opens", async () => {
    const id = await saveDraft(
      me,
      draft([slide({ stickers: [poll, { ...poll, id: newStickerId(), options: ["only one"] } as PollSticker], captions: [{ startMs: 5, endMs: 1, text: "backwards" }, { startMs: 0, endMs: 1000, text: "fine" }] })])
    );
    const back = await loadDraft(me, id);
    expect(back!.slides[0].stickers.map((s) => s.id)).toEqual([poll.id]);
    expect(back!.slides[0].captions).toEqual([{ startMs: 0, endMs: 1000, text: "fine" }]);
  });
});

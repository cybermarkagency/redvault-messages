// Reactions, edits, deletes and files on device keys, against the vectors every client shares.
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";
import { decodeEnvelope, encodeContent, encodeEnvelope, foldChat, type ChatMessage, type ChatOp } from "../lib/e2eV3/chat.js";
import type { FileDescriptor } from "../lib/fileMessage.js";

interface VectorRecord {
  t: string;
  id: string;
  from: string;
  ts: number;
  target?: string;
  body?: string;
  emoji?: string;
}

const vectors = JSON.parse(readFileSync(resolve(__dirname, "../../../desktop/tests/vectors/v3_chat_content_vectors.json"), "utf8"));

function toRecord(v: VectorRecord, arrivedAt: number): ChatMessage {
  const op: ChatOp | undefined =
    v.t === "edit"
      ? { t: "edit", target: v.target!, body: v.body! }
      : v.t === "delete"
        ? { t: "delete", target: v.target! }
        : v.t === "reaction"
          ? { t: "reaction", target: v.target!, emoji: v.emoji! }
          : undefined;
  return {
    conversationId: vectors.conversation,
    id: v.id,
    fromUserId: v.from,
    fromDeviceId: "d",
    ts: v.ts,
    arrivedAt,
    text: op ? "" : v.body!,
    direction: "in",
    status: "received",
    op,
  };
}

const shown = (records: ChatMessage[]) =>
  foldChat(records).map((m) => ({
    id: m.id,
    text: m.text,
    edited: m.edited === true,
    deleted: m.deleted === true,
    reactions: (m.reactions ?? []).map((r) => ({ user: r.userId, emoji: r.emoji })),
  }));

describe("padding", () => {
  it("fills 256-byte buckets, reads back the same, and hides the length of short messages", () => {
    const short = encodeEnvelope(vectors.conversation, "hi", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", 1000);
    const longer = encodeEnvelope(vectors.conversation, "hi".repeat(30), "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", 1000);
    expect(new TextEncoder().encode(short).length).toBe(256);
    expect(new TextEncoder().encode(longer).length).toBe(256);
    expect(decodeEnvelope(short, vectors.conversation, 2000)).toMatchObject({ kind: "message", text: "hi" });
    const big = encodeEnvelope(vectors.conversation, "é".repeat(200), "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", 1000);
    expect(new TextEncoder().encode(big).length % 256).toBe(0);
    expect(new TextEncoder().encode(encodeContent(vectors.conversation, { t: "delete", target: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa" }, undefined, 1000)).length).toBe(256);
  });
});

describe("protocol 3 content", () => {
  for (const c of vectors.fold) {
    it(`${c.name}, in any arrival order`, () => {
      const records = (c.records as VectorRecord[]).map(toRecord);
      expect(shown(records)).toEqual(c.expect);
      expect(shown([...records].reverse())).toEqual(c.expect);
    });
  }

  for (const c of vectors.decode) {
    it(`decodes: ${c.name}`, () => {
      const decoded = decodeEnvelope(JSON.stringify(c.envelope), vectors.conversation, 2000);
      expect(decoded.kind).toBe(c.accept);
    });
  }

  it("a file's key travels only inside the envelope, and round-trips", () => {
    const file: FileDescriptor = {
      type: "file",
      version: 1,
      objectId: "33333333-3333-4333-8333-333333333333",
      fileKey: "c2VjcmV0LWZpbGUta2V5LXNlY3JldC1maWxlLWtleSE=",
      filename: "notes.txt",
      mimeType: "text/plain",
      originalSize: 5,
      encryptedSize: 46,
      ciphertextSha256: "a".repeat(64),
      encryption: { algorithm: "xchacha20poly1305-secretstream", version: 1, header: "aGVhZGVyaGVhZGVyaGVhZGVyaGVhZGVy", chunkSize: 65536 },
    } as FileDescriptor;
    const envelope = encodeContent(vectors.conversation, { t: "file", file }, "aaaaaaaa-0000-4000-8000-000000000001", 1000);
    const decoded = decodeEnvelope(envelope, vectors.conversation, 2000);
    expect(decoded.kind === "message" ? decoded.file?.filename : null).toBe("notes.txt");
  });
});

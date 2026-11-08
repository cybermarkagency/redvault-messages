// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { beforeAll, describe, expect, it, vi } from "vitest";
import * as api from "../lib/api.js";
import * as crypto from "../lib/e2eCrypto.js";
import { startMessageRequest } from "../state/actions.js";

vi.mock("../lib/api.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/api.js")>()),
  getUser: vi.fn(),
  createConversation: vi.fn(async () => ({ id: "11111111-1111-4111-8111-111111111111", isGroup: false, createdAt: "2026-10-19T00:00:00.000Z" })),
}));

let me: { userId: string; handle: string; publicKey: Uint8Array; privateKey: Uint8Array };
let creator: { userId: string; publicKey: Uint8Array; privateKey: Uint8Array };

beforeAll(async () => {
  await crypto.ready();
  me = { ...crypto.generateKeyPair(), userId: "00000000-0000-4000-8000-0000000000a1", handle: "fan" };
  creator = { ...crypto.generateKeyPair(), userId: "00000000-0000-4000-8000-0000000000c1" };
});

describe("messaging a creator who charges for messages", () => {
  it("opens a 1:1 request with one conversation key sealed to each side, then shows the thread", async () => {
    vi.mocked(api.getUser).mockResolvedValue({ userId: creator.userId, username: "maker", publicKey: crypto.toBase64(creator.publicKey), avatarUrl: null } as never);
    const dispatch = vi.fn();
    const id = await startMessageRequest(dispatch, me as never, creator.userId, "off" as never);

    const sent = vi.mocked(api.createConversation).mock.calls[0][0];
    expect(sent.isGroup).toBe(false);
    expect(sent.participants.map((p) => p.userId)).toEqual([me.userId, creator.userId]);
    // Each side opens the same key with its own private key: nothing readable for the server.
    const mine = crypto.unsealConversationKey(crypto.fromBase64(sent.participants[0].wrappedKey), me.publicKey, me.privateKey);
    const theirs = crypto.unsealConversationKey(crypto.fromBase64(sent.participants[1].wrappedKey), creator.publicKey, creator.privateKey);
    expect(theirs).toEqual(mine);

    expect(dispatch.mock.calls.map(([a]) => a.type)).toEqual(["CONTACT_READY", "OPEN_THREAD"]);
    expect(dispatch.mock.calls[1][0]).toEqual({ type: "OPEN_THREAD", conversationId: id });
    expect(dispatch.mock.calls[0][0].contact).toMatchObject({ kind: "dm", userId: creator.userId, handle: "maker" });
  });
});

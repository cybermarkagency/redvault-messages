// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import "fake-indexeddb/auto";
import { render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { TorGate } from "../components/TorGate.js";
import * as crypto from "../lib/e2eCrypto.js";
import * as outbox from "../lib/outbox.js";
import { isNetworkFailure, TOR_UNAVAILABLE_MESSAGE } from "../lib/torRouting.js";
import { createInitialState, reducer } from "../state/reducer.js";
import type { Message } from "../types.js";

// Tor routing on the web (docs/tor-routing-plan.md): the Tor Browser gate on the
// clearnet address, the sealed outbox, and queued messages in the chat.

async function accountKey(seed: number): Promise<Uint8Array> {
  await crypto.ready();
  return outbox.outboxKey(new Uint8Array(32).fill(seed));
}

describe("the outbox", () => {
  it("keeps items sealed, in order, and only for its own account", async () => {
    const key = await accountKey(1);
    const ids = [outbox.newClientMessageId(), outbox.newClientMessageId(), outbox.newClientMessageId()];
    for (const [n, id] of ids.entries()) await outbox.addText(key, id, "chat-1", `message ${n}`);
    const items = await outbox.items(key);
    expect(items.map((i) => i.text)).toEqual(["message 0", "message 1", "message 2"]);
    // Another account's key opens none of them.
    expect(await outbox.items(await accountKey(2))).toEqual([]);
    for (const id of ids) await outbox.remove(id);
    expect(await outbox.items(key)).toEqual([]);
  });

  it("stores nothing readable", async () => {
    const key = await accountKey(3);
    const id = outbox.newClientMessageId();
    await outbox.addText(key, id, "chat-secret-7f3a", "the launch code is 4471", {
      messageId: "m1",
      senderId: "u2",
      preview: "a quote nobody may read",
      fromMe: false,
    });
    const raw = await new Promise<{ sealed: string }[]>((resolve) => {
      const req = indexedDB.open("redvault-outbox", 1);
      req.onsuccess = () => {
        const get = req.result.transaction("items").objectStore("items").getAll();
        get.onsuccess = () => {
          resolve(get.result);
          req.result.close();
        };
      };
    });
    const text = raw.map((r) => new TextDecoder("latin1").decode(crypto.fromBase64(r.sealed)) + r.sealed).join("");
    for (const needle of ["launch code", "a quote nobody may read", "chat-secret-7f3a"]) expect(text).not.toContain(needle);
    await outbox.remove(id);
  });

  it("holds a refused item until Retry", async () => {
    const key = await accountKey(4);
    const item = await outbox.addText(key, outbox.newClientMessageId(), "chat-1", "hello");
    await outbox.recordAttempt(key, item, TOR_UNAVAILABLE_MESSAGE, false);
    let [now] = await outbox.items(key);
    expect(now.attempts).toBe(1);
    expect(now.held).toBe(false);
    now = await outbox.recordAttempt(key, now, "Refused", true);
    expect((await outbox.items(key))[0].held).toBe(true);
    await outbox.release(key, now);
    expect((await outbox.items(key))[0].held).toBe(false);
    await outbox.remove(item.id);
  });

  it("uses ids the server accepts as clientMessageIds", () => {
    // server/src/routes/conversationRoutes.ts: ^[A-Za-z0-9_-]{16,64}$
    for (let i = 0; i < 20; i++) expect(outbox.newClientMessageId()).toMatch(/^[A-Za-z0-9_-]{16,64}$/);
  });

  it("derives the same key as the desktop and Android apps", async () => {
    const sodium = await crypto.ready();
    const account = new Uint8Array(32).map((_, i) => i * 7);
    expect(await outbox.outboxKey(account)).toEqual(sodium.crypto_kdf_derive_from_key(32, 1, "rvoutbx1", account));
  });

  it("tells a dropped connection from a refusal", () => {
    expect(isNetworkFailure(new TypeError("NetworkError when attempting to fetch resource."))).toBe(true);
    expect(isNetworkFailure(new Error("blocked"))).toBe(false);
  });
});

describe("queued messages in the chat", () => {
  const queued: Message = { from: "me", text: "waiting", time: "10:00", createdAt: 5, queueId: "q-1" };

  it("shows them, keeps them across refreshes, and replaces them once sent", () => {
    let state = createInitialState();
    state = {
      ...state,
      conversations: [{ kind: "dm", id: "c1", handle: "bob", messages: [], time: "", unread: 0 } as never],
    };
    state = reducer(state, { type: "OUTBOX_LOADED", queued: [{ conversationId: "c1", message: queued }], held: 0 });
    expect(state.conversations[0].messages.map((m) => m.queueId)).toEqual(["q-1"]);
    expect(state.outbox.queued).toBe(1);

    // A poll brings the server's messages: the queued one stays.
    state = reducer(state, { type: "MESSAGES_LOADED", conversationId: "c1", messages: [{ id: "s1", from: "them", text: "hi", time: "", createdAt: 1 }] });
    expect(state.conversations[0].messages.map((m) => m.id ?? m.queueId)).toEqual(["s1", "q-1"]);

    // Sent from the outbox: the stored message replaces the queued copy.
    state = reducer(state, {
      type: "MESSAGE_SENT",
      conversationId: "c1",
      replacesQueueId: "q-1",
      message: { id: "s2", from: "me", text: "waiting", time: "", createdAt: 6 },
    });
    expect(state.conversations[0].messages.map((m) => m.id ?? m.queueId)).toEqual(["s1", "s2"]);
  });

  it("knows when the connection is down", () => {
    const state = reducer(createInitialState(), { type: "NETWORK_STATE", offline: true });
    expect(state.outbox.offline).toBe(true);
  });
});

describe("the Tor Browser gate on the clearnet address", () => {
  afterEach(() => vi.unstubAllGlobals());

  function routing(mode: string) {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () =>
        new Response(JSON.stringify({ mode, onionAddress: "a".repeat(52) + "aaad.onion", cutoffAt: "2027-01-01T00:00:00Z", viaOnion: false }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        })
      )
    );
  }

  it("enforce: the app doesn't run here; it says where to go instead", async () => {
    routing("enforce");
    render(
      <TorGate>
        <p>the app</p>
      </TorGate>
    );
    expect(await screen.findByText("Open Redvault in Tor Browser")).toBeInTheDocument();
    expect(screen.queryByText("the app")).not.toBeInTheDocument();
    expect(screen.getByText(/aaad\.onion/)).toBeInTheDocument();
  });

  it("notice: the app runs, with the move announced", async () => {
    routing("notice");
    render(
      <TorGate>
        <p>the app</p>
      </TorGate>
    );
    expect(await screen.findByText(/Redvault is moving to Tor/)).toBeInTheDocument();
    expect(screen.getByText("the app")).toBeInTheDocument();
  });

  it("off: nothing changes", async () => {
    routing("off");
    render(
      <TorGate>
        <p>the app</p>
      </TorGate>
    );
    expect(screen.getByText("the app")).toBeInTheDocument();
    await new Promise((r) => setTimeout(r, 20));
    expect(screen.queryByText(/Tor/)).not.toBeInTheDocument();
  });
});

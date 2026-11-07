import { describe, expect, it, vi } from "vitest";
import { CallSignalError, openCallSignal, sealCallSignal, type CallSignal } from "../lib/callCrypto.js";
import * as crypto from "../lib/e2eCrypto.js";
import { gatherComplete, relayExpiryMs, renewalDelayMs, type GatheringSource } from "../lib/turnRenewal.js";

// TURN credential renewal for long one-to-one calls (lib/turnRenewal.ts, decision 5).

describe("TURN credential renewal", () => {
  it("reads the expiry from the TURN REST username, earliest first", () => {
    expect(relayExpiryMs([{ username: "1792500000:ab12cd34ef56ab78" }])).toBe(1792500000 * 1000);
    expect(relayExpiryMs([{ username: "1792500300:x" }, { username: "1792500000:y" }])).toBe(1792500000 * 1000);
    expect(relayExpiryMs([{ username: "static-user" }, {}])).toBeNull();
  });

  it("renews at 80% of what's left, never sooner than a minute", () => {
    const now = 1_000_000;
    expect(renewalDelayMs(now + 24 * 3600_000, now)).toBe(Math.floor(24 * 3600_000 * 0.8));
    expect(renewalDelayMs(now + 30_000, now)).toBe(60_000);
    expect(renewalDelayMs(now - 5_000, now)).toBe(60_000);
  });

  it("seals the ICE restart like the first offer, and refuses one meant for something else", async () => {
    await crypto.ready();
    const caller = crypto.generateKeyPair();
    const callee = crypto.generateKeyPair();
    const restart: CallSignal = {
      v: 1,
      type: "restart-offer",
      callId: "call-1",
      conversationId: "chat-1",
      from: "u-caller",
      to: "u-callee",
      kind: "voice",
      sdp: "v=0\r\na=fingerprint:sha-256 AA:BB\r\n",
      sentAt: Date.now(),
    };
    const sealed = sealCallSignal(restart, callee.publicKey, caller.privateKey);
    const expected = { callId: "call-1", conversationId: "chat-1", from: "u-caller", to: "u-callee", kind: "voice" as const };
    expect(openCallSignal(sealed, caller.publicKey, callee.privateKey, { ...expected, type: "restart-offer" }).sdp).toBe(restart.sdp);
    // An ICE restart can't stand in for the call's first offer (or the other way round).
    expect(() => openCallSignal(sealed, caller.publicKey, callee.privateKey, { ...expected, type: "offer" })).toThrow(CallSignalError);
    // Nor be opened as if from someone else.
    expect(() => openCallSignal(sealed, crypto.generateKeyPair().publicKey, callee.privateKey, { ...expected, type: "restart-offer" })).toThrow(
      CallSignalError
    );
  });
});

/** A connection stand-in whose gathering state and local SDP the test moves by hand. */
function fakeConnection(state: RTCIceGathererState, sdp = ""): GatheringSource & { move(next: RTCIceGathererState, sdp?: string): void } {
  const target = new EventTarget();
  const pc = {
    iceGatheringState: state,
    localDescription: { sdp } as RTCSessionDescription,
    addEventListener: target.addEventListener.bind(target),
    removeEventListener: target.removeEventListener.bind(target),
    move(next: RTCIceGathererState, nextSdp?: string) {
      (pc as { iceGatheringState: RTCIceGathererState }).iceGatheringState = next;
      if (nextSdp !== undefined) pc.localDescription = { sdp: nextSdp } as RTCSessionDescription;
      target.dispatchEvent(new Event("icegatheringstatechange"));
    },
  };
  return pc as unknown as GatheringSource & { move(next: RTCIceGathererState, sdp?: string): void };
}

describe("waiting for ICE gathering", () => {
  it("returns at once for a first offer when gathering is already complete", async () => {
    await expect(gatherComplete(fakeConnection("complete"), 1000)).resolves.toBeUndefined();
  });

  it("after an ICE restart, 'complete' before gathering has started again doesn't count (the bug that dropped every restart)", async () => {
    vi.useFakeTimers();
    try {
      // What the browser showed right after setLocalDescription(restart offer): complete, and no relay candidate yet.
      const pc = fakeConnection("complete", "v=0\r\na=ice-ufrag:new\r\n");
      let resolved = false;
      void gatherComplete(pc, 6000, true).then(() => (resolved = true));
      await vi.advanceTimersByTimeAsync(50);
      expect(resolved).toBe(false);
      pc.move("gathering");
      await vi.advanceTimersByTimeAsync(50);
      expect(resolved).toBe(false);
      pc.move("complete", "v=0\r\na=candidate:1 1 udp 1 203.0.113.9 50000 typ relay raddr 0.0.0.0 rport 0\r\n");
      await vi.advanceTimersByTimeAsync(0);
      expect(resolved).toBe(true);
    } finally {
      vi.useRealTimers();
    }
  });

  it("after a restart, a description that already holds its relay candidates is done", async () => {
    const pc = fakeConnection("complete", "a=candidate:1 1 udp 1 203.0.113.9 50000 typ relay raddr 0.0.0.0 rport 0\r\n");
    await expect(gatherComplete(pc, 1000, true)).resolves.toBeUndefined();
  });

  it("gives up after the timeout if gathering never moves", async () => {
    vi.useFakeTimers();
    try {
      const pc = fakeConnection("complete", "");
      let resolved = false;
      void gatherComplete(pc, 6000, true).then(() => (resolved = true));
      await vi.advanceTimersByTimeAsync(5999);
      expect(resolved).toBe(false);
      await vi.advanceTimersByTimeAsync(2);
      expect(resolved).toBe(true);
    } finally {
      vi.useRealTimers();
    }
  });
});

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

/**
 * Live end-to-end check of paid communities: this app's real community
 * crypto against a running API over HTTP. Skipped unless COMMUNITIES_E2E_API
 * is set, e.g. COMMUNITIES_E2E_API=http://localhost:3088. That API must be a
 * throwaway one (in-memory repositories, communities enabled, creator age
 * gate 0) whose payment gateway is a Stripe stand-in like
 * server/src/test/fakes/fakeCommunityGateway.ts, extended so onboarding
 * completes at once and POST <checkoutUrl>/pay pays the checkout and delivers
 * the signed checkout.session.completed and invoice.paid webhooks through the
 * real webhook handler. (The harness used during development isn't in the
 * repository.) Never point it at a real server.
 */
import { describe, expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import { buildKeyRing, decryptForCommunity, encryptForCommunity, newCommunityKey, prepareEnrollments, prepareRotation, type KeyStateDto } from "../lib/communityCrypto.js";

const API = process.env.COMMUNITIES_E2E_API;

interface Client {
  userId: string;
  token: string;
  publicKey: Uint8Array;
  privateKey: Uint8Array;
  call<T = unknown>(method: string, path: string, body?: unknown): Promise<{ status: number; body: T }>;
}

async function register(username: string): Promise<Client> {
  await crypto.ready();
  const keys = crypto.generateKeyPair();
  const wrapped = crypto.wrapPrivateKey(keys.privateKey, "e2e-test-pass-1");
  const res = await fetch(`${API}/api/auth/register`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      username,
      password: "e2e-test-pass-1",
      publicKey: crypto.toBase64(keys.publicKey),
      encryptedPrivateKey: crypto.toBase64(wrapped.encryptedPrivateKey),
      privateKeyNonce: crypto.toBase64(wrapped.nonce),
      privateKeyKdfSalt: crypto.toBase64(wrapped.kdfSalt),
      terms_accepted: true,
      privacy_acknowledged: true,
      age_18_confirmed: true,
    }),
  });
  expect(res.status).toBe(201);
  const token = /session_token=([^;]+)/.exec(res.headers.getSetCookie().join(";"))![1];
  const me = (await res.json()) as { userId: string };
  const call = async <T,>(method: string, path: string, body?: unknown) => {
    const r = await fetch(`${API}/api${path}`, {
      method,
      headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}` },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const text = await r.text();
    return { status: r.status, body: (text ? JSON.parse(text) : undefined) as T };
  };
  return { userId: me.userId, token, ...keys, call };
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

describe.skipIf(!API)("paid communities, live (client crypto + server + Stripe stand-in)", () => {
  it("a paid subscriber reads the creator's posts; once removed, a new key keeps them out", { timeout: 60_000 }, async () => {
    const suffix = Math.random().toString(36).slice(2, 8);
    const creator = await register(`maker${suffix}`);
    const fan = await register(`fan${suffix}`);

    // Creator profile, paid memberships, payouts (the stand-in completes onboarding at once).
    expect((await creator.call("POST", "/communities/creator", { handle: `nm_${suffix}`, displayName: "Night Market", acceptTerms: true })).status).toBe(201);
    expect((await creator.call("PATCH", "/communities/creator/me", { paidMembershipsEnabled: true })).status).toBe(200);
    expect((await creator.call("POST", "/communities/creator/me/onboarding", { country: "us" })).status).toBe(200);
    const refreshed = await creator.call<{ creator: { payments: { ready: boolean } } }>("POST", "/communities/creator/me/onboarding/refresh");
    expect(refreshed.body.creator.payments.ready).toBe(true);

    // A paid channel whose epoch-0 key is made and sealed here.
    const communityId = globalThis.crypto.randomUUID();
    const { wrappedKey } = newCommunityKey(communityId, creator.publicKey);
    const created = await creator.call<{ id: string; conversationId: string }>("POST", "/communities", {
      id: communityId,
      kind: "channel",
      name: `Notes ${suffix}`,
      category: "education",
      visibility: "discoverable",
      accessType: "paid",
      priceMinor: 1000,
      wrappedKey,
    });
    expect(created.status).toBe(201);
    expect(created.body.id).toBe(communityId);
    const conversationId = created.body.conversationId;
    expect((await creator.call("POST", `/communities/${communityId}/publish`)).status).toBe(200);

    const ringOf = async (client: Client) => {
      const state = await client.call<KeyStateDto>("GET", `/communities/${communityId}/keys`);
      expect(state.status).toBe(200);
      return buildKeyRing(communityId, state.body, client);
    };
    const post = async (client: Client, text: string) => {
      const ring = await ringOf(client);
      const sealed = encryptForCommunity(ring, text);
      return client.call<{ id: string }>("POST", `/conversations/${conversationId}/messages`, {
        ciphertext: crypto.toBase64(sealed.ciphertext),
        nonce: crypto.toBase64(sealed.nonce),
        keyEpoch: sealed.keyEpoch,
      });
    };
    expect((await post(creator, "first post, epoch 0")).status).toBe(201);

    // The fan subscribes: checkout at the stand-in, which pays and sends signed webhooks.
    const checkout = await fan.call<{ checkoutUrl: string; subscriptionId: string }>("POST", `/communities/${communityId}/checkout`, { platform: "web", termsAccepted: true, ageConfirmed: true });
    expect(checkout.status).toBe(200);
    expect((await fan.call("GET", `/communities/${communityId}/keys`)).status).toBe(404);
    const pay = await fetch(`${checkout.body.checkoutUrl}/pay`, { method: "POST", redirect: "manual" });
    expect(pay.status).toBe(303);
    let entitlement = "pending";
    for (let i = 0; i < 20 && entitlement === "pending"; i++) {
      await sleep(500);
      entitlement = (await fan.call<{ entitlementStatus: string }>("POST", `/community-subscriptions/${checkout.body.subscriptionId}/refresh`)).body.entitlementStatus;
    }
    expect(entitlement).toBe("active");

    // The creator's app delivers the current key to the waiting fan.
    const pending = await creator.call<{ userId: string; publicKey: string }[]>("GET", `/communities/${communityId}/enrollments`);
    expect(pending.body.map((p) => p.userId)).toEqual([fan.userId]);
    for (const e of prepareEnrollments(await ringOf(creator), pending.body)) {
      expect((await creator.call("POST", `/communities/${communityId}/enrollments`, e)).status).toBe(204);
    }

    // The fan reads the creator's post with their own key.
    const fanRing0 = await ringOf(fan);
    expect((await post(creator, "second post, epoch 0")).status).toBe(201);
    const listed = await fan.call<{ id: string; ciphertext: string; nonce: string; keyEpoch: number }[]>("GET", `/conversations/${conversationId}/messages`);
    expect(listed.status).toBe(200);
    const texts = listed.body.map((m) => decryptForCommunity(fanRing0, m.keyEpoch, crypto.fromBase64(m.ciphertext), crypto.fromBase64(m.nonce)));
    expect(texts).toContain("second post, epoch 0");
    // Subscribers of a channel can't post.
    expect((await post(fan, "hello?")).status).toBe(403);

    // Removed: no more access, and nothing goes out under a key they hold.
    expect((await creator.call("POST", `/communities/${communityId}/members/${fan.userId}/remove`, {})).status).toBe(204);
    expect((await fan.call("GET", `/conversations/${conversationId}/messages`)).status).toBe(404);
    expect((await post(creator, "after removal, old key")).status).toBe(409);
    const roster = await creator.call<{ keyEpoch: number; members: { userId: string; publicKey: string }[] }>("GET", `/communities/${communityId}/roster`);
    expect(roster.body.members.map((m) => m.userId)).toEqual([creator.userId]);
    const rotation = prepareRotation(await ringOf(creator), roster.body);
    expect((await creator.call("POST", `/communities/${communityId}/rotate`, { fromEpoch: rotation.fromEpoch, keys: rotation.keys })).status).toBe(200);
    const secret = await post(creator, "after removal, epoch 1");
    expect(secret.status).toBe(201);
    const creatorView = await creator.call<{ id: string; ciphertext: string; nonce: string; keyEpoch: number }[]>("GET", `/conversations/${conversationId}/messages`);
    const newest = creatorView.body.find((m) => m.id === secret.body.id)!;
    expect(newest.keyEpoch).toBe(1);
    // Even if the removed fan got hold of this ciphertext, their saved keys don't open it.
    expect(decryptForCommunity(fanRing0, newest.keyEpoch, crypto.fromBase64(newest.ciphertext), crypto.fromBase64(newest.nonce))).toBeNull();
    for (const key of fanRing0.keys.values()) {
      expect(() => crypto.decryptWithConversationKey(crypto.fromBase64(newest.ciphertext), crypto.fromBase64(newest.nonce), key)).toThrow();
    }
    // The creator still reads everything.
    const creatorRing = await ringOf(creator);
    expect(decryptForCommunity(creatorRing, 1, crypto.fromBase64(newest.ciphertext), crypto.fromBase64(newest.nonce))).toBe("after removal, epoch 1");
  });
});

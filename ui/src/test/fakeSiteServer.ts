// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// A stand-in for the server, at the fetch level, for the website's tests:
// just the routes the website uses (sign-in, plans, the transparency feed,
// the admin panel, community memberships). Every request is recorded, so a
// test can also check what the website never asks for (messages, device
// keys, the live connection).
import { vi } from "vitest";
import type { PlanStatus } from "../lib/api.js";
import type { SubscriptionTier, UserRole } from "../types.js";

export interface FakeUser {
  userId: string;
  username: string;
  password: string;
  role: UserRole;
  tier: SubscriptionTier;
  mfa: boolean;
}

export const MFA_CODE = "123456";
export const PRO_PAYMENT_LINK = "https://buy.stripe.com/test_pro";
export const PREMIUM_PAYMENT_LINK = "https://buy.stripe.com/test_premium";
export const COMMUNITY_ID = "0b6f8f0e-1c2d-4e5f-8a9b-0c1d2e3f4a5b";
const GiB = 1024 * 1024 * 1024;

interface State {
  users: FakeUser[];
  session: string | null;
  pendingMfa: string | null;
  plans: Map<string, PlanStatus>;
  requests: { method: string; url: string; body: string }[];
}

export const fake: State = { users: [], session: null, pendingMfa: null, plans: new Map(), requests: [] };

export function resetFakeSite(): void {
  fake.users = [];
  fake.session = null;
  fake.pendingMfa = null;
  fake.plans = new Map();
  fake.requests = [];
}

export function addUser(user: Partial<FakeUser> & { username: string }): FakeUser {
  const full: FakeUser = { userId: `user-${fake.users.length + 1}`, password: "correct horse battery staple", role: "user", tier: "free", mfa: false, ...user };
  fake.users.push(full);
  return full;
}

/** Signed in already, as if from an earlier visit (the session cookie). */
export function signInAs(user: FakeUser): void {
  fake.session = user.userId;
}

function planFor(user: FakeUser): PlanStatus {
  const existing = fake.plans.get(user.userId);
  if (existing && existing.tier === user.tier) return existing;
  const status: PlanStatus =
    user.tier === "free"
      ? { tier: "free", source: null, renews: false, endsOn: null, canCancel: false, canResume: false, canSwitchTo: null }
      : { tier: user.tier, source: "stripe", renews: true, endsOn: null, canCancel: true, canResume: false, canSwitchTo: user.tier === "pro" ? "premium" : "pro" };
  fake.plans.set(user.userId, status);
  return status;
}

const json = (status: number, body: unknown) =>
  status === 204 ? new Response(null, { status }) : new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } });

function authResult(user: FakeUser) {
  return {
    userId: user.userId,
    acceptMessageRequests: true,
    transparencyMode: false,
    avatarUrl: null,
    expiresAt: new Date(Date.now() + 3600_000).toISOString(),
    role: user.role,
    subscriptionTier: user.tier,
    publicKey: "AAAA",
    encryptedPrivateKey: "AAAA",
    privateKeyNonce: "AAAA",
    privateKeyKdfSalt: "AAAA",
  };
}

function route(method: string, fullPath: string, body: Record<string, unknown>): Response {
  const path = fullPath.split("?")[0];
  const me = fake.users.find((u) => u.userId === fake.session) ?? null;
  const key = `${method} ${path}`;
  switch (key) {
    case "GET /api/public/routing":
      return json(404, { error: "not_found" });
    case "POST /api/auth/prelogin":
      return json(200, { kdfSalt: "AAAAAAAAAAAAAAAAAAAAAA==", authVersion: 1, upgrade: false });
    case "POST /api/auth/login": {
      const user = fake.users.find((u) => u.username.toLowerCase() === String(body.username).toLowerCase());
      if (!user || user.password !== body.password) return json(401, { error: "invalid_credentials", message: "Invalid credentials" });
      if (user.mfa) {
        fake.pendingMfa = user.userId;
        return json(200, { mfaRequired: true });
      }
      fake.session = user.userId;
      return json(200, authResult(user));
    }
    case "POST /api/auth/mfa/verify": {
      const user = fake.users.find((u) => u.userId === fake.pendingMfa);
      if (!user || body.code !== MFA_CODE) return json(401, { error: "invalid_code", message: "That code isn't right." });
      fake.pendingMfa = null;
      fake.session = user.userId;
      return json(200, authResult(user));
    }
    case "GET /api/auth/me":
      if (!me) return json(401, { error: "unauthorized" });
      return json(200, { ...authResult(me), username: me.username, mfaEnabled: me.mfa });
    case "POST /api/auth/logout":
      fake.session = null;
      return json(204, null);
    case "GET /api/public/transparency-feed":
      return json(200, []);
    case "GET /api/billing/config":
      return json(200, { paymentLink: PRO_PAYMENT_LINK, premiumPaymentLink: PREMIUM_PAYMENT_LINK, filesStorage: { free: 5 * GiB, pro: 50 * GiB, premium: 100 * GiB } });
  }
  if (path.startsWith("/api/billing/") || path.startsWith("/api/admin/") || path.startsWith("/api/community")) {
    if (!me) return json(401, { error: "unauthorized" });
  }
  switch (key) {
    case "GET /api/billing/status":
      return json(200, planFor(me!));
    case "POST /api/billing/switch": {
      me!.tier = body.tier as SubscriptionTier;
      return json(200, planFor(me!));
    }
    case "POST /api/billing/cancel": {
      const next = { ...planFor(me!), renews: false, canCancel: false, canResume: true, endsOn: "2026-12-09" };
      fake.plans.set(me!.userId, next);
      return json(200, next);
    }
    case "GET /api/community-subscriptions":
      return json(200, []);
    case "GET /api/communities/config":
      return json(200, { enabled: true, paymentsEnabled: true, purchasePlatforms: ["web"], categories: [] });
  }
  if (method === "POST" && /^\/api\/community-subscriptions\/[^/]+\/refresh$/.test(path)) {
    return json(200, { id: path.split("/")[3], entitlementStatus: "active" });
  }
  if (method === "GET" && path === `/api/communities/${COMMUNITY_ID}`) {
    return json(200, {
      id: COMMUNITY_ID,
      name: "Night Owls",
      description: "A paid group.",
      kind: "group",
      category: "other",
      code: "NIGHT1",
      status: "published",
      newMembersPaused: false,
      accessType: "paid",
      price: { amountMinor: 499, currency: "usd", interval: "month" },
      memberCount: 12,
      creator: { id: "c1", handle: "owl", displayName: "Owl", verified: true },
      viewer: { membership: null, subscription: null, isStaff: false },
    });
  }
  if (path.startsWith("/api/admin/")) {
    if (me!.role !== "super_admin") return json(403, { error: "forbidden" });
    if (key === "GET /api/admin/users")
      return json(
        200,
        fake.users.map((u) => ({ userId: u.userId, username: u.username, role: u.role, subscriptionTier: u.tier, createdAt: "2026-11-01T00:00:00Z", deletedAt: null }))
      );
    if (path.startsWith("/api/admin/reports")) return json(200, []);
  }
  return json(404, { error: "not_found" });
}

/** Routes this page's fetch calls to the fake. */
export function installFakeSite(): void {
  vi.stubGlobal(
    "fetch",
    vi.fn(async (input: RequestInfo | URL, init: RequestInit = {}) => {
      const url = typeof input === "string" ? input : input instanceof URL ? input.href : input.url;
      const method = (init.method ?? "GET").toUpperCase();
      const raw = typeof init.body === "string" ? init.body : "";
      fake.requests.push({ method, url, body: raw });
      const path = url.replace(/^https?:\/\/[^/]+/, "");
      let body: Record<string, unknown> = {};
      try {
        body = raw ? (JSON.parse(raw) as Record<string, unknown>) : {};
      } catch {
        body = {};
      }
      return route(method, path.split("#")[0], body);
    })
  );
}

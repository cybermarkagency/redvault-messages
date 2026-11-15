// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Thin fetch wrapper for the server, for what the website still does:
// signing in, the plan and its payments, the public transparency feed and
// the admin panel. Requests go to API_BASE (lib/apiBase.ts), on this site's
// own origin, so the session cookie stays same-origin.
//
// Deliberately absent: conversations, messages, device keys, calls, files,
// stories, channels and the live connection. Messaging lives in the apps.
import { API_BASE } from "./apiBase.js";
import type { SubscriptionTier, UserRole } from "../types.js";

export class ApiError extends Error {
  constructor(
    public readonly status: number,
    public readonly code: string,
    message: string
  ) {
    super(message);
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, {
    ...init,
    credentials: "include",
    headers: { "Content-Type": "application/json", ...init?.headers },
  });

  if (res.status === 204) return undefined as T;

  const body = await res.json().catch(() => null);
  if (!res.ok) {
    throw new ApiError(res.status, body?.error ?? "unknown_error", body?.message ?? res.statusText);
  }
  return body as T;
}

// --- Signing in ---

/** A new account, as the admin panel creates it (the same shape as sign-up in the apps). */
export interface RegisterInput {
  username: string;
  password: string;
  publicKey: string;
  encryptedPrivateKey: string;
  privateKeyNonce: string;
  privateKeyKdfSalt: string;
  /** 2: `password` is the sign-in key (lib/authKey.ts). */
  authVersion?: 2;
  kdfOps?: number;
  kdfMem?: number;
  /** The sign-up form's three boxes, as ticked; the server refuses the account unless all are true. */
  terms_accepted: boolean;
  privacy_acknowledged: boolean;
  age_18_confirmed: boolean;
}

export interface AuthResult {
  userId: string;
  acceptMessageRequests: boolean;
  transparencyMode: boolean;
  /** Absent on older servers. */
  showOnlineStatus?: boolean;
  avatarUrl: string | null;
  expiresAt: string;
  role: UserRole;
  subscriptionTier: SubscriptionTier;
}

export interface LoginResult extends AuthResult {
  publicKey: string;
  encryptedPrivateKey: string;
  privateKeyNonce: string;
  privateKeyKdfOps?: number;
  privateKeyKdfMem?: number;
  privateKeyKdfSalt: string;
}

/** Returned instead of LoginResult when the account has MFA enabled — no key material is included until /auth/mfa/verify succeeds. */
export interface MfaRequiredResult {
  mfaRequired: true;
}

export interface PreLoginResult {
  kdfSalt: string;
  kdfOps?: number;
  kdfMem?: number;
  authVersion: 1 | 2;
  upgrade: boolean;
}

/** The salt and version needed to derive the sign-in key. Answers for any username. */
export function preLogin(username: string): Promise<PreLoginResult> {
  return request("/auth/prelogin", { method: "POST", body: JSON.stringify({ username }) });
}

export function login(
  username: string,
  credential: { password: string; authVersion?: 2; upgradeAuthKey?: string }
): Promise<LoginResult | MfaRequiredResult> {
  return request("/auth/login", { method: "POST", body: JSON.stringify({ username, ...credential }) });
}

/** Completes login after a `login()` call returned `{mfaRequired: true}` — reads the pending session directly off the cookie, no requireAuth. Returns the same shape a normal login would, key material included. */
export function verifyMfaCode(code: string): Promise<LoginResult> {
  return request("/auth/mfa/verify", { method: "POST", body: JSON.stringify({ code }) });
}

export function logout(): Promise<void> {
  return request("/auth/logout", { method: "POST" });
}

export interface MeResult {
  userId: string;
  username: string;
  publicKey: string;
  acceptMessageRequests: boolean;
  transparencyMode: boolean;
  /** Opt-in, reciprocal online status. Absent on older servers. */
  showOnlineStatus?: boolean;
  avatarUrl: string | null;
  role: UserRole;
  subscriptionTier: SubscriptionTier;
  mfaEnabled: boolean;
  hasRecoveryKey?: boolean;
}

export function me(): Promise<MeResult> {
  return request("/auth/me");
}

// --- Public live transparency feed ---
//
// Unauthenticated, no cookies needed — see server/src/routes/transparencyRoutes.ts.
// `ciphertext` is populated only for the fixed sandbox conversation, or for
// a message whose sender has Transparency Mode on (their own explicit
// opt-in) — never plaintext, never a key.

export interface PublicTransparencyEventDto {
  publicTransactionId: string;
  commitment: string;
  protocolVersion: number;
  relayStatus: "relayed";
  timestamp: string;
  ciphertext: string | null;
  expired: boolean;
}

export function getTransparencyFeed(options?: { limit?: number; before?: string }): Promise<PublicTransparencyEventDto[]> {
  const params = new URLSearchParams();
  if (options?.limit) params.set("limit", String(options.limit));
  if (options?.before) params.set("before", options.before);
  const qs = params.toString() ? `?${params.toString()}` : "";
  return request(`/public/transparency-feed${qs}`);
}

// --- Admin (requireAuth + requireSuperAdmin server-side) ---

export interface AnnouncementReplyDto {
  id: string;
  userId: string;
  /** Set on the admin's side only. */
  username?: string;
  body: string;
  createdAt: string;
}

export interface AdminBroadcastDto {
  id: string;
  kind: "message" | "poll";
  body: string;
  allowReplies: boolean;
  createdAt: string;
  recipients: number;
  read: number;
  replyCount: number;
  options: { id: string; label: string; votes: number }[];
}

export interface SendBroadcastInput {
  kind: "message" | "poll";
  body: string;
  options: string[];
  allowReplies: boolean;
  recipientIds: string[];
}

export function adminSendBroadcast(input: SendBroadcastInput): Promise<AdminBroadcastDto> {
  return request("/admin/broadcasts", { method: "POST", body: JSON.stringify(input) });
}

export function adminListBroadcasts(): Promise<AdminBroadcastDto[]> {
  return request("/admin/broadcasts");
}

export function adminGetBroadcast(id: string): Promise<AdminBroadcastDto & { replies: AnnouncementReplyDto[] }> {
  return request(`/admin/broadcasts/${id}`);
}

export interface AdminUserDto {
  userId: string;
  username: string;
  role: UserRole;
  subscriptionTier: SubscriptionTier;
  createdAt: string;
  deletedAt: string | null;
}

export function adminListUsers(options?: { limit?: number; offset?: number }): Promise<AdminUserDto[]> {
  const params = new URLSearchParams();
  if (options?.limit) params.set("limit", String(options.limit));
  if (options?.offset) params.set("offset", String(options.offset));
  const qs = params.toString() ? `?${params.toString()}` : "";
  return request(`/admin/users${qs}`);
}

export function adminCreateUser(input: RegisterInput): Promise<{ userId: string }> {
  return request("/admin/users", { method: "POST", body: JSON.stringify(input) });
}

export function adminDeleteUser(userId: string): Promise<void> {
  return request(`/admin/users/${userId}`, { method: "DELETE" });
}

export function adminSetTier(userId: string, tier: SubscriptionTier): Promise<void> {
  return request(`/admin/users/${userId}/tier`, { method: "PATCH", body: JSON.stringify({ tier }) });
}

export type ReportStatus = "open" | "reviewed" | "dismissed";

export interface AdminReportDto {
  id: string;
  reporterId: string;
  reportedId: string;
  reason: string;
  status: ReportStatus;
  createdAt: string;
  reviewedAt: string | null;
  reviewedBy: string | null;
  /** Convenience fields the admin listing may denormalize for display — not guaranteed, so callers should fall back to the raw id. */
  reporterUsername?: string;
  reportedUsername?: string;
}

export function adminListReports(status?: ReportStatus): Promise<AdminReportDto[]> {
  const qs = status ? `?status=${encodeURIComponent(status)}` : "";
  return request(`/admin/reports${qs}`);
}

export function adminResolveReport(reportId: string, status: "reviewed" | "dismissed"): Promise<void> {
  return request(`/admin/reports/${reportId}`, { method: "PATCH", body: JSON.stringify({ status }) });
}

// --- Billing (Stripe; the webhook, not the browser, is what upgrades an account) ---

export function getBillingConfig(): Promise<{
  paymentLink: string | null;
  premiumPaymentLink?: string | null;
  /** Each plan's Files storage in bytes; null when the server has no Files. */
  filesStorage?: { free: number; pro: number; premium: number } | null;
}> {
  return request("/billing/config");
}

/** Where the account's plan is headed; see server/src/services/billingService.ts's PlanStatus. */
export interface PlanStatus {
  tier: SubscriptionTier;
  source: "stripe" | "crypto" | "granted" | null;
  renews: boolean;
  /** Last day of Pro (YYYY-MM-DD) when nothing renews it. */
  endsOn: string | null;
  canCancel: boolean;
  canResume: boolean;
  /** The card subscription can move to this plan from Settings (Pro to Premium or back). Absent on older servers. */
  canSwitchTo?: "pro" | "premium" | null;
}

export function getPlanStatus(): Promise<PlanStatus> {
  return request("/billing/status");
}

/** Switch to Free: the card subscription stops renewing; Pro lasts until the paid period ends. */
export function cancelPlan(): Promise<PlanStatus> {
  return request("/billing/cancel", { method: "POST" });
}

/** Undoes cancelPlan() before the period ends. */
export function resumePlan(): Promise<PlanStatus> {
  return request("/billing/resume", { method: "POST" });
}

/** Moves a card subscription between Pro and Premium; Stripe charges or credits the difference. */
export function switchPlan(tier: "pro" | "premium"): Promise<PlanStatus> {
  return request("/billing/switch", { method: "POST", body: JSON.stringify({ tier }) });
}

/** The payment link with this account's id attached, so Stripe's webhook can tell the server which account paid. */
export function paymentLinkFor(paymentLink: string, userId: string): string {
  const url = new URL(paymentLink);
  url.searchParams.set("client_reference_id", userId);
  return url.toString();
}

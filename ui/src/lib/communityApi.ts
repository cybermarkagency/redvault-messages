// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Paid communities: the server's /api/communities, /api/community-subscriptions
// and /api/admin/communities (docs/paid-communities.md). Every amount comes
// from the server in integer minor units; this app never decides a price,
// a commission or whether a payment succeeded.
import { ApiError } from "./api.js";
import { API_BASE } from "./apiBase.js";

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, {
    ...init,
    credentials: "include",
    headers: { "Content-Type": "application/json", ...init?.headers },
  });
  if (res.status === 204) return undefined as T;
  const body = await res.json().catch(() => null);
  if (!res.ok) throw new ApiError(res.status, body?.error ?? "unknown_error", body?.message ?? res.statusText);
  return body as T;
}

const post = <T>(path: string, body?: unknown) => request<T>(path, { method: "POST", body: body === undefined ? undefined : JSON.stringify(body) });
const patch = <T>(path: string, body: unknown) => request<T>(path, { method: "PATCH", body: JSON.stringify(body) });
const id = encodeURIComponent;

export type CommunityKind = "channel" | "group";
export type CommunityRole = "owner" | "admin" | "moderator" | "member";
export type MembershipStatus = "pending_approval" | "pending_enrollment" | "active" | "removed" | "banned" | "left" | "expired";
export type EntitlementStatus = "pending" | "active" | "trialing" | "grace_period" | "cancel_scheduled" | "expired" | "canceled" | "refunded" | "disputed" | "suspended";

export interface PriceDto {
  id?: string;
  amountMinor: number;
  currency: string;
  interval: string;
  version?: number;
}

export interface CreatorPublicDto {
  id: string;
  handle: string;
  displayName: string;
  bio?: string;
  profileLink?: string | null;
  hasAvatar?: boolean;
  verified: boolean;
}

export interface MembershipDto {
  status: MembershipStatus;
  role: CommunityRole;
  canPost: boolean;
  source: string;
  enrolledAt: string | null;
}

export interface SubscriptionDto {
  id: string;
  communityId: string;
  billingStatus: string;
  entitlementStatus: EntitlementStatus;
  cancelAtPeriodEnd: boolean;
  currentPeriodEnd: string | null;
  accessUntil: string | null;
  graceUntil: string | null;
  renewsOn: string | null;
  provider: string;
  termsVersion: string;
  createdAt: string;
  endedAt: string | null;
}

export interface CommunityListingDto {
  id: string;
  code: string;
  kind: CommunityKind;
  name: string;
  description: string;
  category: string;
  rules: string;
  previewText: string;
  hasAvatar: boolean;
  hasBanner: boolean;
  visibility: "unlisted" | "discoverable";
  accessType: "free" | "paid";
  status: "draft" | "published" | "unpublished" | "closing" | "closed" | "suspended";
  newMembersPaused: boolean;
  publishedAt: string | null;
  creator: CreatorPublicDto;
  price: PriceDto | null;
  memberCount: number | null;
}

export interface CommunityStaffDto {
  memberLimit: number | null;
  joinApproval: boolean;
  invitationsSuspended: boolean;
  showMemberCount: boolean;
  slowModeSeconds: number;
  keyEpoch: number;
  rekeyRequired: boolean;
  version: number;
  createdAt: string;
}

export interface CommunityPageDto extends CommunityListingDto {
  conversationId: string | null;
  viewer: { membership: MembershipDto | null; isStaff: boolean; subscription: SubscriptionDto | null };
  staff: CommunityStaffDto | null;
}

export interface CommunitiesConfigDto {
  enabled: boolean;
  categories: string[];
  paymentsEnabled: boolean;
  purchasePlatforms: string[];
  currency: string;
  minPriceMinor: number;
  maxPriceMinor: number;
  commissionBps: number;
  creatorCountries: string[];
  graceDays: number;
  creatorTermsVersion: string;
  subscriberTermsVersion: string;
}

export interface MembershipViewDto extends SubscriptionDto {
  community: { id: string; name: string; kind: CommunityKind; status: string; code: string; hasAvatar: boolean };
  creatorHandle: string | null;
  price: PriceDto | null;
}

export interface ReportDto {
  id: string;
  communityId: string | null;
  reporterId: string | null;
  targetType: string;
  targetUserId: string | null;
  category: string;
  details: string;
  evidenceMessageId: string | null;
  evidencePlaintext: string | null;
  evidenceCiphertextSha256: string | null;
  platformReview: boolean;
  status: "open" | "actioned" | "dismissed";
  resolutionNote: string | null;
  evidencePurgedAt: string | null;
  createdAt: string;
  resolvedAt: string | null;
}

export type ClientPlatform = "web" | "desktop";

// Config, creator profile, payouts

export const getCommunitiesConfig = () => request<CommunitiesConfigDto>("/communities/config");

// Discovery and communities

export const getCommunity = (communityId: string, invite?: string) =>
  request<CommunityPageDto>(`/communities/${id(communityId)}${invite ? `?invite=${id(invite)}` : ""}`);
export const startCommunityCheckout = (communityId: string, input: { platform: ClientPlatform; termsAccepted: boolean; ageConfirmed: boolean; inviteToken?: string }) =>
  post<{ subscriptionId: string; checkoutUrl: string; reused: boolean }>(`/communities/${id(communityId)}/checkout`, input);

// Members and moderation

// Key epochs

// The member's own subscriptions

export const listMyMemberships = () => request<MembershipViewDto[]>("/community-subscriptions");
export const refreshMembership = (subscriptionId: string) => post<MembershipViewDto>(`/community-subscriptions/${id(subscriptionId)}/refresh`);
export const cancelMembership = (subscriptionId: string) => post<MembershipViewDto>(`/community-subscriptions/${id(subscriptionId)}/cancel`);
export const resumeMembership = (subscriptionId: string) => post<MembershipViewDto>(`/community-subscriptions/${id(subscriptionId)}/resume`);
export const membershipPortal = (subscriptionId: string) => post<{ url: string }>(`/community-subscriptions/${id(subscriptionId)}/portal`);
export const membershipReceipts = (subscriptionId: string) =>
  request<{ id: string; kind: string; currency: string; amountMinor: number; taxMinor: number; periodStart: string | null; periodEnd: string | null; occurredAt: string }[]>(
    `/community-subscriptions/${id(subscriptionId)}/receipts`
  );

// RedVault administrators

export interface AdminTransactionDto {
  id: string;
  kind: string;
  communityId: string;
  currency: string;
  grossAmountMinor: number;
  taxAmountMinor: number;
  commissionBps: number;
  commissionAmountMinor: number;
  creatorAmountMinor: number;
  processingFeeMinor: number | null;
  reconciliationStatus: "pending" | "matched" | "mismatch";
  reconciliationNote: string | null;
  occurredAt: string;
}

export interface CommissionRuleDto {
  id: string;
  version: number;
  rateBps: number;
  currency: string;
  minPriceMinor: number;
  maxPriceMinor: number;
  effectiveFrom: string;
  note: string;
  createdAt: string;
}

export interface AdminOverviewDto {
  platformCommissionNetMinor: number;
  processingFeesMinor: number;
  refundLossesMinor: number;
  disputeLossesMinor: number;
  providerClearingMinor: number;
  unbalancedJournals: number;
  mismatches: number;
  rules: CommissionRuleDto[];
  totals: { kind: string; currency: string; count: number; grossMinor: number; commissionMinor: number; creatorMinor: number; processingFeeMinor: number }[];
}

export const adminCommunityOverview = () => request<AdminOverviewDto>("/admin/communities/overview");
export const adminCommunityTransactions = (reconciliation?: "pending" | "matched" | "mismatch") =>
  request<AdminTransactionDto[]>(`/admin/communities/transactions${reconciliation ? `?reconciliation=${reconciliation}` : ""}`);
export const adminRefundCommunityPayment = (transactionId: string, amountMinor: number | null, reason: string) =>
  post<{ refundId: string }>(`/admin/communities/transactions/${id(transactionId)}/refund`, { amountMinor, reason });
export const adminAddCommissionRule = (input: { rateBps: number; minPriceMinor: number; maxPriceMinor: number; note: string }) =>
  post<CommissionRuleDto>("/admin/communities/commission-rules", input);
export const adminCommunityCreators = () =>
  request<(CreatorPublicDto & { userId: string; status: string; createdAt: string; payments: { status: string; transfersStatus: string; requirementsDue: boolean; livemode: boolean } | null })[]>(
    "/admin/communities/creators"
  );
export const adminSetCreator = (creatorId: string, change: { status?: "active" | "suspended"; verified?: boolean }) =>
  patch<CreatorPublicDto>(`/admin/communities/creators/${id(creatorId)}`, change);
export const adminCommunities = (status?: string) =>
  request<(Omit<CommunityListingDto, "creator" | "price" | "memberCount"> & CommunityStaffDto & { creatorId: string })[]>(
    `/admin/communities/communities${status ? `?status=${status}` : ""}`
  );
export const adminCommunityAction = (communityId: string, action: "suspend" | "unsuspend" | "close") =>
  post<{ status: string }>(`/admin/communities/communities/${id(communityId)}/${action}`);
export const adminCommunityReports = (status?: "open" | "actioned" | "dismissed") =>
  request<ReportDto[]>(`/admin/communities/reports${status ? `?status=${status}` : ""}`);
export const adminResolveCommunityReport = (reportId: string, status: "actioned" | "dismissed", note: string | null) =>
  post<void>(`/admin/communities/reports/${id(reportId)}/resolve`, { status, note });
export const adminReconcileCommunities = () => post<{ checked: number; mismatches: number; unbalancedJournals: number }>("/admin/communities/reconcile");

// Money, for display only (amounts are always the server's).

const EXPONENT: Record<string, number> = { usd: 2 };

export function formatMoney(amountMinor: number, currency: string): string {
  const exponent = EXPONENT[currency.toLowerCase()] ?? 2;
  try {
    return new Intl.NumberFormat(undefined, { style: "currency", currency: currency.toUpperCase(), minimumFractionDigits: exponent, maximumFractionDigits: exponent }).format(amountMinor / 10 ** exponent);
  } catch {
    return `${currency.toUpperCase()} ${(amountMinor / 10 ** exponent).toFixed(exponent)}`;
  }
}

/** "12%" / "12.5%" from basis points. */
export function formatRate(bps: number): string {
  return `${Number((bps / 100).toFixed(2))}%`;
}

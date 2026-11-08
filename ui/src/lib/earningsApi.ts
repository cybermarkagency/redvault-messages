// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Creator earnings: the server's /api/earnings (docs/creator-earnings.md):
// paid messages, tips and exclusive posts. Every price and fee comes from
// the server; this app never decides an amount (beyond the tip a person
// picks, which the server checks) or whether a payment went through.
import { ApiError } from "./api.js";
import { API_BASE } from "./apiBase.js";

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${API_BASE}/earnings${path}`, {
    ...init,
    credentials: "include",
    headers: { "Content-Type": "application/json", ...init?.headers },
  });
  if (res.status === 204) return undefined as T;
  const body = await res.json().catch(() => null);
  if (!res.ok) throw new ApiError(res.status, body?.error ?? "unknown_error", body?.message ?? res.statusText);
  return body as T;
}

const send = <T>(method: string, path: string, body?: unknown) => request<T>(path, { method, body: body === undefined ? undefined : JSON.stringify(body) });
const id = encodeURIComponent;

export type EarningProduct = "paid_message" | "tip" | "exclusive_post";
export type PaidMessageMode = "off" | "first_contact" | "per_message";
export type PurchaseStatus = "checkout_pending" | "paid" | "consuming" | "consumed" | "refund_pending" | "refunded" | "disputed" | "expired" | "canceled" | "failed";

export interface EarningsConfigDto {
  enabled: boolean;
  purchasePlatforms: ("web" | "desktop")[];
  currency: string;
  tipPresetsMinor: number[];
  postMedia: boolean;
  rules: Record<EarningProduct, { ratePercent: number; minMinor: number; maxMinor: number }>;
}

export interface EarningSettingsDto {
  available: boolean;
  creatorReady: boolean;
  paidMessagesMode: PaidMessageMode;
  paidMessagePriceMinor: number | null;
  tipsEnabled: boolean;
  acceptsMessageRequests: boolean;
  exemptions: string[];
}

export interface OfferDto {
  available: boolean;
  creator?: { id: string; handle: string; displayName: string };
  paidMessages: { mode: PaidMessageMode; priceMinor: number; currency: string } | null;
  tips: boolean;
  posts: number;
}

export interface QuoteDto {
  required: boolean;
  reason: string;
  mode: PaidMessageMode;
  priceMinor: number | null;
  currency: string;
  purchasable: boolean;
  availablePurchaseId: string | null;
  safetyContactAvailable: boolean;
}

export interface PurchaseDto {
  id: string;
  product: EarningProduct;
  status: PurchaseStatus;
  amountMinor: number;
  currency: string;
  commissionPercent: number;
  platformFeeMinor: number;
  creatorUserId: string | null;
  postId: string | null;
  paidMessageMode: "first_contact" | "per_message" | null;
  showName: boolean;
  /** Creator's view only: who paid, when they let it show. */
  buyer?: { userId: string; username: string } | null;
  createdAt: string;
  paidAt: string | null;
  refundedAt: string | null;
  useBy: string | null;
  keyDelivered: boolean;
}

export interface PostDto {
  id: string;
  authorId: string;
  creatorId: string;
  priceMinor: number;
  currency: string;
  teaser: string;
  status: "draft" | "published" | "unpublished" | "removed";
  createdAt: string;
  hasMedia: boolean;
  mediaSize: number | null;
  mine: boolean;
  owned: boolean;
  awaitingKey: boolean;
  locked: boolean;
  body: { ciphertext: string; nonce: string; wrappedKey: string } | null;
  creator?: { handle: string; displayName: string } | null;
  purchasedAt?: string | null;
}

export interface DashboardDto {
  available: boolean;
  creatorReady: boolean;
  products: {
    product: EarningProduct;
    commissionPercent: number;
    count: number;
    grossMinor: number;
    platformFeeMinor: number;
    creatorNetMinor: number;
    refundedMinor: number;
    refunds: number;
    disputes: number;
  }[];
  totals: { grossMinor: number; platformFeeMinor: number; creatorNetMinor: number };
  transactions: PurchaseDto[];
  awaitingKeys: number;
  payouts: {
    balance: { available: { currency: string; amountMinor: number }[]; pending: { currency: string; amountMinor: number }[] } | null;
    recent: { id: string; amountMinor: number; currency: string; status: string; arrivalDate: string | null }[];
  };
  settings: EarningSettingsDto;
}

export interface KeyRequestDto {
  purchaseId: string;
  postId: string;
  buyerId: string;
  buyerPublicKey: string;
}

export const getEarningsConfig = () => request<EarningsConfigDto>("/config");
export const getEarningSettings = () => request<EarningSettingsDto>("/settings");
export const updateEarningSettings = (s: { paidMessagesMode?: PaidMessageMode; paidMessagePriceMinor?: number | null; tipsEnabled?: boolean }) =>
  send<EarningSettingsDto>("PUT", "/settings", s);
export const setExemption = (userId: string, exempt: boolean) => send<void>(exempt ? "PUT" : "DELETE", `/exemptions/${id(userId)}`);
export const getOffer = (userId: string) => request<OfferDto>(`/creators/${id(userId)}`);
export const getQuote = (userId: string, conversationId?: string) =>
  request<QuoteDto>(`/quote/${id(userId)}${conversationId ? `?conversationId=${id(conversationId)}` : ""}`);

export interface StartPurchase {
  product: EarningProduct;
  creatorUserId?: string;
  postId?: string;
  amountMinor?: number;
  showName?: boolean;
  /** Generated once per purchase attempt, so a retried request never starts a second checkout. */
  idempotencyKey: string;
  platform: "web" | "desktop";
}

export const startPurchase = (p: StartPurchase) => send<{ purchaseId: string; status: PurchaseStatus; checkoutUrl: string | null }>("POST", "/purchases", p);
export const getPurchase = (purchaseId: string) => request<PurchaseDto>(`/purchases/${id(purchaseId)}`);
export const listMyPurchases = () => request<PurchaseDto[]>("/purchases");
export const refundPurchase = (purchaseId: string) => send<{ status: string }>("POST", `/purchases/${id(purchaseId)}/refund`);
export const getEarningsDashboard = () => request<DashboardDto>("/dashboard");
export const listTipsReceived = () => request<PurchaseDto[]>("/tips");

export const createPost = (p: { id: string; priceMinor: number; teaser: string; bodyCiphertext: string; bodyNonce: string; wrappedKey: string; mediaSize: number | null }) =>
  send<{ id: string; status: string }>("POST", "/posts", p);
export async function uploadPostMedia(postId: string, ciphertext: Blob): Promise<void> {
  const res = await fetch(`${API_BASE}/earnings/posts/${id(postId)}/media`, {
    method: "PUT",
    credentials: "include",
    headers: { "Content-Type": "application/octet-stream" },
    body: ciphertext,
  });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new ApiError(res.status, body?.error ?? "unknown_error", body?.message ?? res.statusText);
  }
}
export async function downloadPostMedia(postId: string): Promise<Uint8Array[]> {
  const res = await fetch(`${API_BASE}/earnings/posts/${id(postId)}/media`, { credentials: "include", cache: "no-store" });
  if (!res.ok) throw new ApiError(res.status, "post_media_unavailable", "This post's media isn't available.");
  return [new Uint8Array(await res.arrayBuffer())];
}
export const getPost = (postId: string) => request<PostDto>(`/posts/${id(postId)}`);
export const listCreatorPosts = (userId: string) => request<PostDto[]>(`/creators/${id(userId)}/posts`);
export const unpublishPost = (postId: string) => send<void>("POST", `/posts/${id(postId)}/unpublish`);
export const getLibrary = () => request<PostDto[]>("/library");
export const getKeyRequests = () => request<KeyRequestDto[]>("/key-requests");
export const deliverPostKey = (postId: string, purchaseId: string, wrappedKey: string) => send<void>("POST", `/posts/${id(postId)}/keys`, { purchaseId, wrappedKey });

/** "$3.00": every amount here is USD cents from the server. */
export function formatUsd(minor: number | null | undefined): string {
  return minor === null || minor === undefined ? "" : `$${(minor / 100).toFixed(2)}`;
}

/** A fresh key for one purchase attempt (kept for retries of that attempt only). */
export function newIdempotencyKey(): string {
  const bytes = new Uint8Array(16);
  globalThis.crypto.getRandomValues(bytes);
  return `p_${Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("")}`;
}

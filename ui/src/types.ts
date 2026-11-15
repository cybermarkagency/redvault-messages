// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

export type UserRole = "user" | "super_admin";
export type SubscriptionTier = "free" | "pro" | "premium";

export const TIER_LABELS: Record<SubscriptionTier, string> = { free: "Free", pro: "Pro", premium: "Premium" };

/**
 * Who is signed in to the website. Only what the plan and admin pages need:
 * the website never unwraps the account's private key (messaging, and every
 * key, lives in the apps).
 */
export interface SiteAccount {
  handle: string;
  userId: string;
  /** Server-assigned. "super_admin" is granted only via a one-off manual DB update, never through any in-app path. */
  role: UserRole;
  subscriptionTier: SubscriptionTier;
  mfaEnabled: boolean;
}

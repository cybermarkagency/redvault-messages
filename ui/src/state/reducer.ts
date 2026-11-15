// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import type { AppLinkKind } from "../lib/siteLinks.js";
import type { SiteAccount, SubscriptionTier } from "../types.js";

/** The website's pages. Messaging, and everything else of the old web app, lives in the apps. */
export type Screen =
  /** Signed out: the landing page. Signed in: the account's first page (the admin panel, or the plan). */
  | { key: "home" }
  | { key: "sign-in" }
  | { key: "transparency" }
  /** Someone opened a link that belongs to the apps. */
  | { key: "app-link"; what: AppLinkKind; communityId?: string; inviteToken?: string }
  /** Back from Stripe's creator payout setup, which is managed in the apps. */
  | { key: "creator-onboarding" }
  | { key: "plans" }
  /** Paid community memberships; `checkoutId` is one just paid for, still being confirmed. */
  | { key: "memberships"; checkoutId?: string; canceled?: boolean }
  /** Buying a community membership in the browser. */
  | { key: "community"; communityId: string; inviteToken?: string }
  | { key: "admin" };

/** Pages that need an account: opening one signed out asks to sign in first, then opens it. */
export const SIGNED_IN_SCREENS: ReadonlySet<Screen["key"]> = new Set(["plans", "memberships", "community", "admin"]);

export type AuthStatus = "checking" | "signed-out" | "authenticating" | "mfa-pending" | "signed-in";

export interface AppState {
  authStatus: AuthStatus;
  authError: string | null;
  identity: SiteAccount | null;
  screen: Screen;
  /** Where to go once signed in. */
  afterSignIn: Screen | null;
}

export type Action =
  | { type: "AUTH_STARTED" }
  | { type: "AUTH_SUCCEEDED"; identity: SiteAccount }
  | { type: "AUTH_FAILED"; message: string }
  | { type: "AUTH_MFA_REQUIRED" }
  | { type: "AUTH_MFA_FAILED"; message: string }
  /** The session cookie was checked on load: still signed in (identity), or not (null). */
  | { type: "SESSION_CHECKED"; identity: SiteAccount | null }
  | { type: "LOGOUT" }
  | { type: "SUBSCRIPTION_TIER_SET"; tier: SubscriptionTier }
  | { type: "NAVIGATE"; screen: Screen }
  /** Back to the start of sign-in (e.g. from the code step). */
  | { type: "SIGN_IN_RESET" };

export function homeFor(identity: SiteAccount): Screen {
  return identity.role === "super_admin" ? { key: "admin" } : { key: "plans" };
}

export function createInitialState(screen: Screen = { key: "home" }): AppState {
  const needsAccount = SIGNED_IN_SCREENS.has(screen.key);
  return {
    authStatus: "checking",
    authError: null,
    identity: null,
    screen: needsAccount ? { key: "sign-in" } : screen,
    afterSignIn: needsAccount ? screen : null,
  };
}

function signedIn(state: AppState, identity: SiteAccount): AppState {
  let next = state.afterSignIn ?? (state.screen.key === "home" || state.screen.key === "sign-in" ? homeFor(identity) : state.screen);
  if (next.key === "admin" && identity.role !== "super_admin") next = homeFor(identity);
  return { ...state, authStatus: "signed-in", authError: null, identity, screen: next, afterSignIn: null };
}

export function reducer(state: AppState, action: Action): AppState {
  switch (action.type) {
    case "AUTH_STARTED":
      return { ...state, authStatus: "authenticating", authError: null };
    case "AUTH_SUCCEEDED":
      return signedIn(state, action.identity);
    case "AUTH_FAILED":
      return { ...state, authStatus: "signed-out", authError: action.message };
    case "AUTH_MFA_REQUIRED":
      return { ...state, authStatus: "mfa-pending", authError: null };
    // A wrong code keeps the code step open instead of starting sign-in over.
    case "AUTH_MFA_FAILED":
      return { ...state, authStatus: "mfa-pending", authError: action.message };
    case "SESSION_CHECKED":
      // Someone may have started signing in while the check was out: theirs wins.
      if (state.authStatus !== "checking") return state;
      if (action.identity) return signedIn(state, action.identity);
      return { ...state, authStatus: "signed-out" };
    case "LOGOUT":
      return { authStatus: "signed-out", authError: null, identity: null, screen: { key: "home" }, afterSignIn: null };
    case "SUBSCRIPTION_TIER_SET":
      return state.identity ? { ...state, identity: { ...state.identity, subscriptionTier: action.tier } } : state;
    case "NAVIGATE": {
      if (SIGNED_IN_SCREENS.has(action.screen.key) && state.authStatus !== "signed-in") {
        return { ...state, screen: { key: "sign-in" }, afterSignIn: action.screen };
      }
      if (action.screen.key === "admin" && state.identity?.role !== "super_admin") return { ...state, screen: { key: "plans" } };
      if (action.screen.key === "sign-in" && state.authStatus === "signed-in") return state;
      return { ...state, screen: action.screen, afterSignIn: action.screen.key === "sign-in" ? state.afterSignIn : null };
    }
    case "SIGN_IN_RESET":
      return { ...state, authStatus: state.authStatus === "signed-in" ? "signed-in" : "signed-out", authError: null };
    default:
      return state;
  }
}

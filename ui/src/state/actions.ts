// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Signing in to the website, and the admin panel's account actions.
//
// Signing in works as in the apps: the password is turned into a sign-in key
// on this device (lib/authKey.ts) and only that is sent, then the 6-digit code
// when the account has two-step verification. Unlike the apps, the website
// never opens the account's private key: nothing here reads messages or
// needs a key, so the wrapped key the server returns is left as it is.

import type { Dispatch } from "react";
import * as api from "../lib/api.js";
import { ApiError } from "../lib/api.js";
import { credentialFor, pinSignInKeys } from "../lib/authKey.js";
import * as crypto from "../lib/e2eCrypto.js";
import { saveLastHandle } from "../lib/lastIdentity.js";
import { isNetworkFailure, unreachableMessage } from "../lib/torRouting.js";
import type { SiteAccount, SubscriptionTier } from "../types.js";
import type { Action } from "./reducer.js";

function describeError(err: unknown, fallback = "Something went wrong."): string {
  if (isNetworkFailure(err)) return unreachableMessage();
  return err instanceof ApiError && err.message ? err.message : fallback;
}

function accountFrom(handle: string, result: api.AuthResult, mfaEnabled: boolean): SiteAccount {
  return { handle, userId: result.userId, role: result.role, subscriptionTier: result.subscriptionTier, mfaEnabled };
}

export async function loginIdentity(dispatch: Dispatch<Action>, handle: string, password: string): Promise<void> {
  dispatch({ type: "AUTH_STARTED" });
  try {
    await crypto.ready();
    const credential = await credentialFor(handle, password);
    const result = await api.login(handle, credential);
    if (credential.authVersion === 2 || credential.upgradeAuthKey) pinSignInKeys(handle);
    if ("mfaRequired" in result) {
      // The password was right; the account also needs its authenticator code.
      dispatch({ type: "AUTH_MFA_REQUIRED" });
      return;
    }
    saveLastHandle(handle);
    dispatch({ type: "AUTH_SUCCEEDED", identity: accountFrom(handle, result, false) });
  } catch (err) {
    // The same message whether the handle doesn't exist or the password is
    // wrong. The one exception: the server was never reached, which says
    // nothing about the account (and over Tor happens routinely).
    dispatch({ type: "AUTH_FAILED", message: isNetworkFailure(err) ? unreachableMessage() : "Wrong handle or password." });
  }
}

/** Finishes a sign-in that asked for the authenticator code. A wrong code keeps the code step open. */
export async function submitMfaCode(dispatch: Dispatch<Action>, handle: string, code: string): Promise<void> {
  try {
    const result = await api.verifyMfaCode(code);
    saveLastHandle(handle);
    dispatch({ type: "AUTH_SUCCEEDED", identity: accountFrom(handle, result, true) });
  } catch (err) {
    dispatch({ type: "AUTH_MFA_FAILED", message: describeError(err, "Wrong code. Try again.") });
  }
}

/** On load: a session cookie from an earlier sign-in in this browser is still good. */
export async function checkSession(dispatch: Dispatch<Action>): Promise<void> {
  try {
    const me = await api.me();
    dispatch({
      type: "SESSION_CHECKED",
      identity: { handle: me.username, userId: me.userId, role: me.role, subscriptionTier: me.subscriptionTier, mfaEnabled: me.mfaEnabled },
    });
  } catch {
    dispatch({ type: "SESSION_CHECKED", identity: null });
  }
}

export async function logout(dispatch: Dispatch<Action>): Promise<void> {
  try {
    await api.logout();
  } catch {
    // Best-effort: this page forgets the session either way.
  }
  dispatch({ type: "LOGOUT" });
}

// --- Admin panel ---

/** What the person ticked on the sign-up form, sent as is: the server decides. */
export interface SignupConsent {
  termsAccepted: boolean;
  privacyAcknowledged: boolean;
  age18Confirmed: boolean;
}

export async function adminCreateAccount(
  handle: string,
  password: string,
  /** Ticked by the admin for the person the account is for; the server refuses it otherwise. */
  consent: SignupConsent
): Promise<{ userId: string }> {
  // The new account's keypair is made and wrapped here, as at sign-up in the
  // apps; it isn't the admin's own session, and nothing of it is kept.
  await crypto.ready();
  const keyPair = crypto.generateKeyPair();
  const wrapped = crypto.wrapPrivateKey(keyPair.privateKey, password);
  return api.adminCreateUser({
    username: handle,
    password,
    publicKey: crypto.toBase64(keyPair.publicKey),
    encryptedPrivateKey: crypto.toBase64(wrapped.encryptedPrivateKey),
    privateKeyNonce: crypto.toBase64(wrapped.nonce),
    privateKeyKdfSalt: crypto.toBase64(wrapped.kdfSalt),
    terms_accepted: consent.termsAccepted,
    privacy_acknowledged: consent.privacyAcknowledged,
    age_18_confirmed: consent.age18Confirmed,
  });
}

export async function adminDeleteAccount(userId: string): Promise<void> {
  await api.adminDeleteUser(userId);
}

export async function adminSetSubscriptionTier(userId: string, tier: SubscriptionTier): Promise<void> {
  await api.adminSetTier(userId, tier);
}

export async function adminResolveReport(reportId: string, status: "reviewed" | "dismissed"): Promise<void> {
  await api.adminResolveReport(reportId, status);
}

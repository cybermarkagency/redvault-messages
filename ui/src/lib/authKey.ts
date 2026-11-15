// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Signing in without giving the server the password (docs/auth-key-split.md).
//
// Argon2id turns the password into the unlock key, which opens the private key
// and never leaves this device. A sign-in key is derived one-way from it, and
// that is all the server sees: it can check who you are, but can't work back
// to the unlock key. The private key stays wrapped exactly as before.
import * as api from "./api.js";
import * as crypto from "./e2eCrypto.js";

export interface Credential {
  password: string;
  authVersion?: 2;
  upgradeAuthKey?: string;
}

const PIN_PREFIX = "redvaultsystem:signInKeys:";
// The server ignores case in usernames, so a pin has to as well.
const pinName = (handle: string) => PIN_PREFIX + handle.trim().toLowerCase();

function pinned(handle: string): boolean {
  try {
    return localStorage.getItem(pinName(handle)) === "1";
  } catch {
    return false;
  }
}

/** This browser saw the account on sign-in keys: from now on it refuses to send the password for it. */
export function pinSignInKeys(handle: string): void {
  try {
    localStorage.setItem(pinName(handle), "1");
  } catch {
    // Best-effort: private browsing, disabled storage, or quota exceeded.
  }
}

function fromPreLogin(pre: api.PreLoginResult, handle: string, password: string): Credential {
  const kdf = pre.kdfOps && pre.kdfMem ? { ops: pre.kdfOps, mem: pre.kdfMem } : crypto.KDF_INTERACTIVE;
  const signInKey = crypto.deriveSignInKey(password, crypto.fromBase64(pre.kdfSalt), kdf);
  if (pre.authVersion === 2) return { password: signInKey, authVersion: 2 };
  if (pinned(handle)) throw new Error("This account's sign-in details look wrong. Your password wasn't sent.");
  return pre.upgrade ? { password, upgradeAuthKey: signInKey } : { password };
}

/** What to send to prove this password, at sign-in or when an action re-checks it. */
export async function credentialFor(handle: string, password: string): Promise<Credential> {
  return fromPreLogin(await api.preLogin(handle), handle, password);
}

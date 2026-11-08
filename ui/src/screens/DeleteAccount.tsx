// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { passwordProof } from "../lib/authKey.js";
import { useState } from "react";
import { TopBar } from "../components/TopBar.js";
import { ApiError, deleteAccount } from "../lib/api.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./FormScreen.module.css";

/** Permanent self-service deletion (also what Google Play's "delete your account" link points people to). */
export function DeleteAccount() {
  const { state, dispatch } = useAppState();
  const [password, setPassword] = useState("");
  const [confirmed, setConfirmed] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const isPro = state.identity?.subscriptionTier === "pro" || state.identity?.subscriptionTier === "premium";

  async function submit() {
    if (password.length === 0 || !confirmed) return;
    setBusy(true);
    setError(null);
    try {
      await deleteAccount(await passwordProof(state.identity!.handle, password));
      dispatch({ type: "LOGOUT" });
    } catch (e) {
      setError(e instanceof ApiError && e.status === 401 ? "Wrong password — try again." : "Couldn't delete the account. Try again.");
      setBusy(false);
    }
  }

  return (
    <div className={styles.screen}>
      <TopBar title="Delete account" onBack={() => dispatch({ type: "OPEN_SETTINGS" })} />
      <div className={styles.body}>
        <p className={`${styles.hint} type-secondary`}>
          This permanently deletes your account and signs you out on every device. Your private key, recovery key,
          two-factor setup, profile photo and notification tokens are removed. You can't undo this, and your username
          can't be used again.
        </p>
        <p className={`${styles.hint} type-secondary`}>
          Messages you already sent stay in the other people's chats, still end-to-end encrypted — the server can't
          read them. They'll see your account as deleted.
        </p>
        {isPro && (
          <p className={`${styles.hint} type-secondary`}>
            You're on Pro: deleting your account doesn't stop billing. Cancel the subscription from your Stripe
            receipt email first.
          </p>
        )}
        {error && <p className={styles.error}>{error}</p>}
        <input
          className={styles.input}
          type="password"
          placeholder="Password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
        />
        <label className="type-secondary" style={{ display: "flex", gap: 8, alignItems: "center" }}>
          <input type="checkbox" checked={confirmed} onChange={(e) => setConfirmed(e.target.checked)} />
          I understand this can't be undone
        </label>
        <button
          type="button"
          className={styles.dangerButton}
          disabled={busy || password.length === 0 || !confirmed}
          onClick={submit}
        >
          {busy ? "Deleting…" : "Delete my account"}
        </button>
      </div>
    </div>
  );
}

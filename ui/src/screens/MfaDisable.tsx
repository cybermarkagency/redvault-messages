// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useState } from "react";
import { TopBar } from "../components/TopBar.js";
import { disableMfa } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./FormScreen.module.css";

/** Disabling requires the account password, not a TOTP code — deliberate, so a lost authenticator app can never be a permanent lockout (this app has no email/SMS recovery path at all). */
export function MfaDisable() {
  const { state, dispatch } = useAppState();
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [done, setDone] = useState(false);

  async function submit() {
    if (password.length === 0) return;
    setBusy(true);
    setError(null);
    try {
      await disableMfa(dispatch, state.identity!.handle, password);
      setDone(true);
    } catch {
      setError("Wrong password — try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.screen}>
      <TopBar title="Turn off two-factor authentication" onBack={() => dispatch({ type: "OPEN_SETTINGS" })} />
      <div className={styles.body}>
        {done ? (
          <p className={styles.success}>Two-factor authentication is now off.</p>
        ) : (
          <>
            <p className={`${styles.hint} type-secondary`}>
              Enter your account password to confirm. This doesn't require a code from your
              authenticator app, so it still works if you've lost access to it.
            </p>
            {error && <p className={styles.error}>{error}</p>}
            <input
              className={styles.input}
              type="password"
              placeholder="Password"
              value={password}
              onChange={(e) => setPassword(e.target.value)}
            />
            <button
              type="button"
              className={styles.dangerButton}
              disabled={busy || password.length === 0}
              onClick={submit}
            >
              {busy ? "Turning off…" : "Turn off two-factor authentication"}
            </button>
          </>
        )}
      </div>
    </div>
  );
}

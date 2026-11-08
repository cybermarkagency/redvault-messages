// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useState } from "react";
import { TopBar } from "../components/TopBar.js";
import { changeUsername } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./FormScreen.module.css";

const MIN_USERNAME_LENGTH = 3;
const MAX_USERNAME_LENGTH = 64; // matches the server's registerSchema

export function ChangeUsername() {
  const { state, dispatch } = useAppState();
  const { identity } = state;
  const [username, setUsername] = useState(identity?.handle ?? "");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [done, setDone] = useState(false);

  if (!identity) return null;

  const trimmed = username.trim();
  const usernameValid = trimmed.length >= MIN_USERNAME_LENGTH && trimmed.length <= MAX_USERNAME_LENGTH;
  const canSubmit = !busy && usernameValid && trimmed !== identity.handle;

  async function submit() {
    if (!canSubmit) return;
    setBusy(true);
    setError(null);
    try {
      await changeUsername(dispatch, trimmed);
      setDone(true);
    } catch {
      setError("That handle couldn't be used — it may already be taken.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.screen}>
      <TopBar title="Change handle" onBack={() => dispatch({ type: "OPEN_SETTINGS" })} />
      <div className={styles.body}>
        <p className={`${styles.hint} type-secondary`}>
          Your handle is how people find and add you. This doesn't affect your keys or existing
          conversations.
        </p>

        {done && <p className={styles.success}>Handle updated to {identity.handle}.</p>}
        {error && <p className={styles.error}>{error}</p>}

        <input
          className={`${styles.input} type-mono`}
          value={username}
          onChange={(e) => setUsername(e.target.value)}
          maxLength={MAX_USERNAME_LENGTH}
          autoCapitalize="none"
          autoCorrect="off"
        />

        <button type="button" className={styles.primaryButton} disabled={!canSubmit} onClick={submit}>
          {busy ? "Saving…" : "Save handle"}
        </button>
      </div>
    </div>
  );
}

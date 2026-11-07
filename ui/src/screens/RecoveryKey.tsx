import { useEffect, useState } from "react";
import { TopBar } from "../components/TopBar.js";
import * as api from "../lib/api.js";
import { createRecoveryKey } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./FormScreen.module.css";

/**
 * Shows a just-created recovery key once (after sign-up, or after making a
 * new one), or lets the user create a new one from Settings. The key only
 * ever exists here, in memory, until the user writes it down.
 */
export function RecoveryKey({ issuedKey }: { issuedKey?: string }) {
  const { state, dispatch } = useAppState();
  const [hasKey, setHasKey] = useState<boolean | null>(null);
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);
  const [saved, setSaved] = useState(false);

  useEffect(() => {
    if (issuedKey) return;
    let cancelled = false;
    api
      .me()
      .then((me) => {
        if (!cancelled) setHasKey(Boolean(me.hasRecoveryKey));
      })
      .catch(() => {
        if (!cancelled) setHasKey(false);
      });
    return () => {
      cancelled = true;
    };
  }, [issuedKey]);

  async function create() {
    if (!state.identity || !password) return;
    setBusy(true);
    setError(null);
    try {
      const key = await createRecoveryKey(state.identity, password);
      setPassword("");
      dispatch({ type: "OPEN_RECOVERY_KEY", issuedKey: key });
    } catch {
      setError("Wrong password — try again.");
    } finally {
      setBusy(false);
    }
  }

  async function copy() {
    if (!issuedKey) return;
    try {
      await navigator.clipboard.writeText(issuedKey);
      setCopied(true);
    } catch {
      setCopied(false);
    }
  }

  if (issuedKey) {
    return (
      <div className={styles.screen}>
        <TopBar title="Your recovery key" />
        <div className={styles.body}>
          <p className={`${styles.hint} type-secondary`}>
            If you ever forget your password, this key is the only way back into your account and messages. Write it
            down or store it in a password manager. It won't be shown again, and nobody (including us) can recover it
            for you.
          </p>
          <p className={`${styles.secretCode} type-mono`} aria-label="Recovery key">
            {issuedKey}
          </p>
          <button type="button" className={styles.secondaryButton} onClick={copy}>
            {copied ? "Copied" : "Copy"}
          </button>
          <label className="type-secondary" style={{ display: "flex", gap: 8, alignItems: "center" }}>
            <input type="checkbox" checked={saved} onChange={(e) => setSaved(e.target.checked)} />
            I've saved my recovery key somewhere safe
          </label>
          <button
            type="button"
            className={styles.primaryButton}
            disabled={!saved}
            onClick={() => dispatch({ type: "GO_CHATS" })}
          >
            Done
          </button>
        </div>
      </div>
    );
  }

  return (
    <div className={styles.screen}>
      <TopBar title="Recovery key" onBack={() => dispatch({ type: "OPEN_SETTINGS" })} />
      <div className={styles.body}>
        <p className={`${styles.hint} type-secondary`}>
          {hasKey === null
            ? "Checking…"
            : hasKey
              ? "Your account has a recovery key. Creating a new one replaces it: the old key stops working."
              : "Your account has no recovery key. Without one, a forgotten password can't be recovered."}
        </p>
        {error && <p className={styles.error}>{error}</p>}
        <input
          className={styles.input}
          type="password"
          placeholder="Current password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
        />
        <button type="button" className={styles.primaryButton} disabled={busy || !password} onClick={create}>
          {busy ? "Creating…" : hasKey ? "Create a new recovery key" : "Create a recovery key"}
        </button>
      </div>
    </div>
  );
}

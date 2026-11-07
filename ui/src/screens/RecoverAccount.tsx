import { useState } from "react";
import { recoverAccount } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./Onboarding.module.css";

const MIN_PASSWORD_LENGTH = 8;

/** "Forgot your password?": recover with the recovery key and choose a new password. */
export function RecoverAccount({ initialHandle, onBack }: { initialHandle: string; onBack: () => void }) {
  const { dispatch } = useAppState();
  const [handle, setHandle] = useState(initialHandle);
  const [key, setKey] = useState("");
  const [password, setPassword] = useState("");
  const [confirm, setConfirm] = useState("");
  const [code, setCode] = useState("");
  const [needsCode, setNeedsCode] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const valid =
    handle.trim().length > 0 &&
    key.trim().length > 0 &&
    password.length >= MIN_PASSWORD_LENGTH &&
    password === confirm &&
    (!needsCode || code.length === 6);

  async function submit() {
    if (!valid || busy) return;
    setBusy(true);
    setError(null);
    try {
      const outcome = await recoverAccount(dispatch, handle.trim(), key, password, needsCode ? code : undefined);
      if (outcome === "code-required") setNeedsCode(true);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Couldn't recover the account. Try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.screen}>
      <div className={`${styles.wordmark} type-wordmark`}>Redvaultsystem</div>
      <p className={`${styles.tagline} type-secondary`}>Recover your account with your recovery key.</p>

      <input className={styles.input} placeholder="Handle" value={handle} onChange={(e) => setHandle(e.target.value)} autoCapitalize="none" />
      <input
        className={`${styles.input} type-mono`}
        placeholder="Recovery key (XXXX-XXXX-…)"
        value={key}
        onChange={(e) => setKey(e.target.value)}
        autoCapitalize="characters"
        autoComplete="off"
        spellCheck={false}
      />
      <input className={styles.input} type="password" placeholder={`New password (min. ${MIN_PASSWORD_LENGTH} characters)`} value={password} onChange={(e) => setPassword(e.target.value)} />
      <input className={styles.input} type="password" placeholder="Confirm new password" value={confirm} onChange={(e) => setConfirm(e.target.value)} />
      {needsCode && (
        <input
          className={styles.input}
          placeholder="6-digit code from your authenticator app"
          inputMode="numeric"
          maxLength={6}
          value={code}
          onChange={(e) => setCode(e.target.value.replace(/\D/g, ""))}
        />
      )}
      {confirm && confirm !== password && <p className={styles.error}>Passwords don't match.</p>}
      {error && <p className={styles.error}>{error}</p>}

      <button type="button" className={styles.continueButton} disabled={!valid || busy} onClick={submit}>
        {busy ? "Recovering…" : "Recover account"}
      </button>
      <button type="button" className={styles.regenerate} onClick={onBack}>
        Back to log in
      </button>
      <p className={`${styles.tagline} type-secondary`} style={{ marginTop: 0 }}>
        This signs out every other device. Your messages stay readable: the recovery key unlocks the same account key.
      </p>
    </div>
  );
}

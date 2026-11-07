import { useState } from "react";
import { TopBar } from "../components/TopBar.js";
import { changePassword } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./FormScreen.module.css";

export function ChangePassword() {
  const { state, dispatch } = useAppState();
  const [currentPassword, setCurrentPassword] = useState("");
  const [newPassword, setNewPassword] = useState("");
  const [confirmPassword, setConfirmPassword] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [done, setDone] = useState(false);

  const { identity } = state;
  if (!identity) return null;

  const canSubmit =
    !busy && currentPassword.length > 0 && newPassword.length >= 8 && newPassword === confirmPassword;

  async function submit() {
    if (!canSubmit || !identity) return;
    setBusy(true);
    setError(null);
    try {
      await changePassword(dispatch, identity, currentPassword, newPassword);
      setDone(true);
      setCurrentPassword("");
      setNewPassword("");
      setConfirmPassword("");
    } catch {
      setError("Couldn't change your password — check your current password and try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.screen}>
      <TopBar title="Change password" onBack={() => dispatch({ type: "OPEN_SETTINGS" })} />
      <div className={styles.body}>
        <p className={`${styles.hint} type-secondary`}>
          Your private key is wrapped with a key derived from your password — changing it
          re-derives and re-wraps that key locally, then sends both together. This signs your
          other devices out.
        </p>

        {done && <p className={styles.success}>Password changed. Other devices have been signed out.</p>}
        {error && <p className={styles.error}>{error}</p>}

        <div className={styles.section}>
          <input
            className={styles.input}
            type="password"
            placeholder="Current password"
            value={currentPassword}
            onChange={(e) => setCurrentPassword(e.target.value)}
          />
          <input
            className={styles.input}
            type="password"
            placeholder="New password (min. 8 characters)"
            value={newPassword}
            onChange={(e) => setNewPassword(e.target.value)}
          />
          <input
            className={styles.input}
            type="password"
            placeholder="Confirm new password"
            value={confirmPassword}
            onChange={(e) => setConfirmPassword(e.target.value)}
          />
        </div>

        <button type="button" className={styles.primaryButton} disabled={!canSubmit} onClick={submit}>
          {busy ? "Changing…" : "Change password"}
        </button>
      </div>
    </div>
  );
}

import { useEffect, useState } from "react";
import QRCode from "qrcode";
import { TopBar } from "../components/TopBar.js";
import { confirmMfaSetup, startMfaSetup } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./FormScreen.module.css";

/** Two-step setup (confirm-before-enable, per the plan) so a typo'd scan can't lock someone out: /auth/mfa/setup issues a pending secret + otpauth URI (rendered here as a QR code via the `qrcode` package — the server never generates an image), then /auth/mfa/confirm promotes it once a valid code is entered. */
export function MfaSetup() {
  const { dispatch } = useAppState();
  const [secret, setSecret] = useState<string | null>(null);
  const [otpauthUri, setOtpauthUri] = useState<string | null>(null);
  const [qrDataUrl, setQrDataUrl] = useState<string | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [code, setCode] = useState("");
  const [busy, setBusy] = useState(false);
  const [confirmError, setConfirmError] = useState<string | null>(null);
  const [done, setDone] = useState(false);

  useEffect(() => {
    let cancelled = false;
    startMfaSetup()
      .then(async (result) => {
        if (cancelled) return;
        setSecret(result.secretBase32);
        setOtpauthUri(result.otpauthUri);
        try {
          const dataUrl = await QRCode.toDataURL(result.otpauthUri);
          if (!cancelled) setQrDataUrl(dataUrl);
        } catch {
          // Falls back to the manual-entry secret below if QR rendering fails.
        }
      })
      .catch(() => {
        if (!cancelled) setLoadError("Couldn't start MFA setup — try again.");
      });
    return () => {
      cancelled = true;
    };
  }, []);

  async function submit() {
    if (code.trim().length === 0) return;
    setBusy(true);
    setConfirmError(null);
    try {
      await confirmMfaSetup(dispatch, code.trim());
      setDone(true);
    } catch {
      setConfirmError("Wrong code — check your authenticator app and try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.screen}>
      <TopBar title="Two-factor authentication" onBack={() => dispatch({ type: "OPEN_SETTINGS" })} />
      <div className={styles.body}>
        {done ? (
          <p className={styles.success}>Two-factor authentication is now on.</p>
        ) : (
          <>
            <p className={`${styles.hint} type-secondary`}>
              Scan this with an authenticator app (like Authy or Google Authenticator), then enter
              the 6-digit code it shows to confirm.
            </p>
            {loadError && <p className={styles.error}>{loadError}</p>}

            {qrDataUrl && (
              <div className={styles.qrWrap}>
                <img src={qrDataUrl} alt="Two-factor authentication QR code" className={styles.qrImage} />
              </div>
            )}
            {secret && !qrDataUrl && !loadError && (
              <p className={`${styles.hint} type-secondary`}>Can't scan? Enter this code manually:</p>
            )}
            {secret && <p className={`${styles.secretCode} type-mono`}>{secret}</p>}
            {!otpauthUri && !loadError && <p className={`${styles.hint} type-secondary`}>Loading…</p>}

            {confirmError && <p className={styles.error}>{confirmError}</p>}
            <input
              className={`${styles.input} type-mono`}
              placeholder="6-digit code"
              inputMode="numeric"
              maxLength={6}
              value={code}
              onChange={(e) => setCode(e.target.value.replace(/\D/g, ""))}
            />
            <button
              type="button"
              className={styles.primaryButton}
              disabled={busy || code.trim().length === 0}
              onClick={submit}
            >
              {busy ? "Confirming…" : "Confirm and turn on"}
            </button>
          </>
        )}
      </div>
    </div>
  );
}

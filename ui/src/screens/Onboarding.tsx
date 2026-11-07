import { RecoverAccount } from "./RecoverAccount.js";
import { type MouseEvent, useEffect, useState } from "react";
import { KeyGlyph } from "../components/KeyGlyph.js";
import { LegalDocumentDialog } from "../components/LegalDocumentDialog.js";
import { SourceCodeLink } from "../components/OpenSourceDialog.js";
import { loadLastHandle } from "../lib/lastIdentity.js";
import type { LegalDocumentId } from "../lib/legalDocuments.js";
import { loginIdentity, registerIdentity, submitMfaCode } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./Onboarding.module.css";

const MIN_USERNAME_LENGTH = 3;
const MAX_USERNAME_LENGTH = 64; // matches the server's registerSchema

export function Onboarding() {
  const { state, dispatch } = useAppState();
  // A reload always loses the in-memory session (no way around that — the
  // private key only ever lives in memory), but if this browser has signed
  // in before, land on Log in with the handle ready instead of defaulting
  // to Create, where it's easy to accidentally register a second account
  // and think the first one "vanished."
  const [lastHandle] = useState(loadLastHandle);
  const [mode, setMode] = useState<"create" | "login" | "recover">(lastHandle ? "login" : "create");
  const [password, setPassword] = useState("");
  const [confirmPassword, setConfirmPassword] = useState("");
  const [loginHandle, setLoginHandle] = useState(lastHandle ?? "");
  const [username, setUsername] = useState(state.onboardingCandidateHandle);
  const [mfaCode, setMfaCode] = useState("");
  const [mfaSubmitting, setMfaSubmitting] = useState(false);
  // The three sign-up boxes: never pre-ticked, and the server refuses the account without all of them.
  const [termsAccepted, setTermsAccepted] = useState(false);
  const [privacyAcknowledged, setPrivacyAcknowledged] = useState(false);
  const [age18Confirmed, setAge18Confirmed] = useState(false);
  const [reading, setReading] = useState<LegalDocumentId | null>(null);

  // "Generate new" replaces the field with a fresh suggestion; typing your
  // own handle afterward isn't touched by this (it only reacts to the
  // candidate itself changing, which only happens on that button click).
  useEffect(() => {
    setUsername(state.onboardingCandidateHandle);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state.onboardingCandidateHandle]);

  const authing = state.authStatus === "authenticating";

  // Password verification succeeded, but the account has MFA enabled — no
  // key material is available yet (see api.ts's LoginResult union). Stays
  // here (rather than either signing in or bouncing back to signed-out on a
  // wrong code — see reducer.ts's AUTH_MFA_FAILED) until a valid 6-digit
  // authenticator code is submitted. Added inline here rather than as a new
  // global Screen since `password` is already held in this component's
  // local state through the whole login flow.
  if (state.authStatus === "mfa-pending") {
    const canSubmitMfa = !mfaSubmitting && mfaCode.trim().length === 6;
    async function submitCode() {
      if (!canSubmitMfa) return;
      setMfaSubmitting(true);
      try {
        await submitMfaCode(dispatch, loginHandle.trim(), password, mfaCode.trim());
      } finally {
        setMfaSubmitting(false);
      }
    }
    return (
      <div className={styles.screen}>
        <div className={`${styles.wordmark} type-wordmark`}>Redvaultsystem</div>
        <p className={`${styles.tagline} type-secondary`}>Enter the 6-digit code from your authenticator app.</p>

        <input
          className={styles.input}
          placeholder="123456"
          inputMode="numeric"
          maxLength={6}
          value={mfaCode}
          onChange={(e) => setMfaCode(e.target.value.replace(/\D/g, ""))}
        />

        {state.authError && <p className={styles.error}>{state.authError}</p>}

        <button type="button" className={styles.continueButton} disabled={!canSubmitMfa} onClick={submitCode}>
          {mfaSubmitting ? "Verifying…" : "Verify"}
        </button>
      </div>
    );
  }

  if (mode === "recover") {
    return <RecoverAccount initialHandle={loginHandle} onBack={() => setMode("login")} />;
  }

  if (mode === "login") {
    const canSubmit = !authing && loginHandle.trim().length > 0 && password.length > 0;
    return (
      <div className={styles.screen}>
        <div className={`${styles.wordmark} type-wordmark`}>Redvaultsystem</div>
        <p className={`${styles.tagline} type-secondary`}>Log back in with your handle and password.</p>

        <input
          className={styles.input}
          placeholder="Handle"
          value={loginHandle}
          onChange={(e) => setLoginHandle(e.target.value)}
          autoCapitalize="none"
          autoCorrect="off"
        />
        <input
          className={styles.input}
          type="password"
          placeholder="Password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
        />

        {state.authError && <p className={styles.error}>{state.authError}</p>}

        <button
          type="button"
          className={styles.continueButton}
          disabled={!canSubmit}
          onClick={() => loginIdentity(dispatch, loginHandle.trim(), password)}
        >
          {authing ? "Logging in…" : "Log in"}
        </button>
        <button
          type="button"
          className={styles.regenerate}
          onClick={() => {
            setMode("create");
            setPassword("");
          }}
        >
          Create a new identity instead
        </button>
        <button type="button" className={styles.regenerate} onClick={() => setMode("recover")}>
          Forgot your password? Use your recovery key
        </button>
        <SourceCodeLink className={styles.feedLink} />
      </div>
    );
  }

  const trimmedUsername = username.trim();
  const usernameValid = trimmedUsername.length >= MIN_USERNAME_LENGTH && trimmedUsername.length <= MAX_USERNAME_LENGTH;
  const allTicked = termsAccepted && privacyAcknowledged && age18Confirmed;
  const canSubmit = !authing && usernameValid && password.length >= 8 && password === confirmPassword && allTicked;

  // A link inside a checkbox's label opens the document without ticking the box.
  const openDocument = (id: LegalDocumentId) => (e: MouseEvent) => {
    e.preventDefault();
    e.stopPropagation();
    setReading(id);
  };

  return (
    <div className={styles.screen}>
      <div className={`${styles.wordmark} type-wordmark`}>Redvaultsystem</div>

      <div className={styles.card}>
        <div className={`${styles.cardTitle} type-title`}>Create Your Identity</div>

        <div className={styles.glyphWrap}>
          <KeyGlyph seed={trimmedUsername || state.onboardingCandidateHandle} size={96} />
        </div>

        <div className={styles.field}>
          <label className={styles.fieldLabel} htmlFor="onboarding-username">
            Username
          </label>
          <input
            id="onboarding-username"
            className={`${styles.input} ${styles.handleInput} type-mono`}
            value={username}
            onChange={(e) => setUsername(e.target.value)}
            placeholder="Choose a handle"
            maxLength={MAX_USERNAME_LENGTH}
            autoCapitalize="none"
            autoCorrect="off"
          />
          <span className={`${styles.fieldLabel} type-caption`} style={{ textTransform: "none" }}>
            e.g. ShadowWolf
          </span>
        </div>
        {!usernameValid && trimmedUsername.length > 0 && (
          <p className={styles.error}>Handles need to be at least {MIN_USERNAME_LENGTH} characters.</p>
        )}

        <button type="button" className={styles.regenerate} onClick={() => dispatch({ type: "REGENERATE_CANDIDATE" })}>
          Suggest one instead
        </button>

        <div className={styles.field}>
          <label className={styles.fieldLabel} htmlFor="onboarding-password">
            Password
          </label>
          <input
            id="onboarding-password"
            className={styles.input}
            type="password"
            placeholder="Choose a password (min. 8 characters)"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
          />
        </div>
        <div className={styles.field}>
          <label className={styles.fieldLabel} htmlFor="onboarding-confirm-password">
            Confirm password
          </label>
          <input
            id="onboarding-confirm-password"
            className={styles.input}
            type="password"
            placeholder="Confirm password"
            value={confirmPassword}
            onChange={(e) => setConfirmPassword(e.target.value)}
          />
        </div>

        <div className={styles.consent} role="group" aria-label="Before you create your account">
          <label className={styles.consentRow}>
            <input type="checkbox" checked={termsAccepted} onChange={(e) => setTermsAccepted(e.target.checked)} />
            <span>
              I agree to the RedVault Messages{" "}
              <button type="button" className={styles.consentLink} onClick={openDocument("terms")}>
                Terms of Service
              </button>
            </span>
          </label>
          <label className={styles.consentRow}>
            <input type="checkbox" checked={privacyAcknowledged} onChange={(e) => setPrivacyAcknowledged(e.target.checked)} />
            <span>
              I acknowledge the RedVault Messages{" "}
              <button type="button" className={styles.consentLink} onClick={openDocument("privacy")}>
                Privacy Policy
              </button>
            </span>
          </label>
          <label className={styles.consentRow}>
            <input type="checkbox" checked={age18Confirmed} onChange={(e) => setAge18Confirmed(e.target.checked)} />
            <span>I confirm I am 18 or older</span>
          </label>
        </div>

        {state.authError && <p className={styles.error}>{state.authError}</p>}

        <button
          type="button"
          className={styles.continueButton}
          disabled={!canSubmit}
          onClick={() =>
            registerIdentity(dispatch, trimmedUsername, password, { termsAccepted, privacyAcknowledged, age18Confirmed })
          }
        >
          {authing ? "Creating…" : "Create Account"}
        </button>

        <p className={styles.disclaimerList}>
          No phone number.
          <br />
          No email address.
          <br />
          No contact book required.
        </p>

        <p className={`${styles.tagline} type-secondary`} style={{ marginTop: 0 }}>
          There is no password reset by email. After you create your account you'll get a recovery key: it's the only
          way back in if you forget your password, so keep it safe.
        </p>
      </div>

      {reading && <LegalDocumentDialog document={reading} onClose={() => setReading(null)} />}

      <button type="button" className={styles.regenerate} onClick={() => setMode("login")}>
        Already have an identity? Log in
      </button>
      <button
        type="button"
        className={styles.feedLink}
        onClick={() => dispatch({ type: "OPEN_TRANSPARENCY_FEED", returnTo: "onboarding" })}
      >
        View the live transparency feed
      </button>
      <SourceCodeLink className={styles.feedLink} />
    </div>
  );
}

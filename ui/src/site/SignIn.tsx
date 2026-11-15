// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { type FormEvent, useState } from "react";
import { WebAssemblyGate } from "../components/WebAssemblyGate.js";
import { loadLastHandle } from "../lib/lastIdentity.js";
import { loginIdentity, submitMfaCode } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./Site.module.css";

/**
 * Sign in with an account made in the apps: the handle and password (sent as
 * a sign-in key, never as the password itself, on accounts that use them),
 * then the authenticator code when two-step verification is on.
 */
export function SignIn() {
  // Signing in derives the sign-in key with libsodium, which is WebAssembly.
  return (
    <WebAssemblyGate>
      <SignInForm />
    </WebAssemblyGate>
  );
}

function SignInForm() {
  const { state, dispatch } = useAppState();
  const [handle, setHandle] = useState(() => loadLastHandle() ?? "");
  const [password, setPassword] = useState("");
  const [code, setCode] = useState("");
  const [verifying, setVerifying] = useState(false);
  const authing = state.authStatus === "authenticating";

  if (state.authStatus === "mfa-pending") {
    const canVerify = !verifying && code.trim().length === 6;
    const verify = async (e: FormEvent) => {
      e.preventDefault();
      if (!canVerify) return;
      setVerifying(true);
      try {
        await submitMfaCode(dispatch, handle.trim(), code.trim());
      } finally {
        setVerifying(false);
      }
    };
    return (
      <div className={`${styles.page} ${styles.narrow}`}>
        <h1 className={`${styles.title} type-display`}>Two-step verification</h1>
        <p className={styles.text}>Enter the 6-digit code from your authenticator app.</p>
        <form className={styles.form} onSubmit={verify}>
          <label className={styles.label}>
            Code
            <input
              className={styles.input}
              placeholder="123456"
              inputMode="numeric"
              autoComplete="one-time-code"
              maxLength={6}
              value={code}
              onChange={(e) => setCode(e.target.value.replace(/\D/g, ""))}
            />
          </label>
          {state.authError && <p className={styles.error}>{state.authError}</p>}
          <div className={styles.actions}>
            <button type="submit" className={styles.primary} disabled={!canVerify}>
              {verifying ? "Verifying…" : "Verify"}
            </button>
            <button
              type="button"
              className={styles.secondary}
              onClick={() => {
                setCode("");
                setPassword("");
                dispatch({ type: "SIGN_IN_RESET" });
              }}
            >
              Start over
            </button>
          </div>
        </form>
      </div>
    );
  }

  const canSubmit = !authing && handle.trim().length > 0 && password.length > 0;
  const submit = (e: FormEvent) => {
    e.preventDefault();
    if (canSubmit) void loginIdentity(dispatch, handle.trim(), password);
  };

  return (
    <div className={`${styles.page} ${styles.narrow}`}>
      <h1 className={`${styles.title} type-display`}>Sign in</h1>
      <p className={styles.text}>Use the handle and password of your RedVault Messages account, to manage your plan or, for site administrators, the admin panel.</p>
      <form className={styles.form} onSubmit={submit}>
        <label className={styles.label}>
          Handle
          <input
            className={styles.input}
            value={handle}
            onChange={(e) => setHandle(e.target.value)}
            autoComplete="username"
            autoCapitalize="none"
            autoCorrect="off"
          />
        </label>
        <label className={styles.label}>
          Password
          <input className={styles.input} type="password" autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} />
        </label>
        {state.authError && <p className={styles.error}>{state.authError}</p>}
        <div className={styles.actions}>
          <button type="submit" className={styles.primary} disabled={!canSubmit}>
            {authing ? "Signing in…" : "Sign in"}
          </button>
        </div>
      </form>
      <p className={styles.muted}>
        No account yet? Accounts are created in the RedVault Messages app. Forgot your password? Use your recovery key in the app.
      </p>
    </div>
  );
}

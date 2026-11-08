// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useState } from "react";
import * as eapi from "../../lib/earningsApi.js";
import styles from "./Earnings.module.css";

/** On someone's profile, when you charge for messages: let them message you for free (or stop). */
export function ExemptionToggle({ userId, name }: { userId: string; name: string }) {
  const [settings, setSettings] = useState<eapi.EarningSettingsDto | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let alive = true;
    eapi.getEarningSettings().then((s) => alive && setSettings(s), () => undefined);
    return () => {
      alive = false;
    };
  }, []);

  if (!settings || settings.paidMessagesMode === "off") return null;
  const exempt = settings.exemptions.includes(userId);

  async function toggle() {
    setBusy(true);
    setError(null);
    try {
      await eapi.setExemption(userId, !exempt);
      setSettings({ ...settings!, exemptions: exempt ? settings!.exemptions.filter((id) => id !== userId) : [...settings!.exemptions, userId] });
    } catch {
      setError("Couldn't change that. Try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.offer}>
      <span className={styles.label}>PAID MESSAGES</span>
      <span className={styles.muted}>
        {exempt ? `${name} can message you for free.` : `${name} pays your message price unless you let them in (friends always can).`}
      </span>
      <button type="button" className={exempt ? styles.secondary : styles.primary} disabled={busy} onClick={toggle}>
        {exempt ? "Charge them again" : `Let ${name} message you free`}
      </button>
      {error && <span className={styles.error}>{error}</span>}
    </div>
  );
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useState } from "react";
import * as api from "../lib/api.js";
import { deviceKeySafetyNumber, EngineError, pendingKeyChange, trustNewKeys, V3Device, type KeyChange } from "../lib/e2eV3/device.js";
import { deviceStorageAvailable } from "../lib/e2eV3/deviceStore.js";
import { engineIncluded } from "../lib/e2eV3/engine.js";
import styles from "../screens/Verify.module.css";

/**
 * The safety number over both accounts' device-key account keys (protocol 3
 * preview). Shown only when the server has it on, this browser has device
 * keys and so does the contact.
 */
export function DeviceKeySafetyNumber({ userId, contactUserId }: { userId: string; contactUserId: string }) {
  const [groups, setGroups] = useState<string[] | null>(null);
  const [problem, setProblem] = useState<string | null>(null);
  const [change, setChange] = useState<KeyChange | null>(null);
  const [reload, setReload] = useState(0);
  const [trusting, setTrusting] = useState(false);

  async function trust() {
    if (!change) return;
    setTrusting(true);
    try {
      const device = await V3Device.open(userId);
      if (!device) return;
      await trustNewKeys(device, contactUserId, change.accountKey);
      setChange(null);
      setProblem(null);
      setReload((n) => n + 1);
    } catch {
      setProblem("Couldn't accept the new keys. Try again.");
    } finally {
      setTrusting(false);
    }
  }

  useEffect(() => {
    if (!engineIncluded() || !deviceStorageAvailable()) return;
    let cancelled = false;
    (async () => {
      const config = await api.getV3Config();
      if (!config.enabled) return;
      const device = await V3Device.open(userId);
      if (!device) return;
      const number = await deviceKeySafetyNumber(device, contactUserId);
      if (!cancelled) setGroups(number);
    })().catch((err) => {
      if (cancelled) return;
      if (err instanceof EngineError && err.code === "account_key_changed") {
        setProblem("Their device keys changed since you last saw them. Compare the new number with them before trusting it.");
        // Show the new key's number, so there is something to compare.
        V3Device.open(userId)
          .then((device) => (device ? pendingKeyChange(device, contactUserId) : null))
          .then((found) => {
            if (!cancelled) setChange(found);
          })
          .catch(() => {});
      }
    });
    return () => {
      cancelled = true;
    };
  }, [userId, contactUserId, reload]);

  if (problem) {
    return (
      <>
        <p className={styles.warning} role="alert">
          {problem}
        </p>
        {change && (
          <>
            <div className={styles.digits} role="img" aria-label={`New device-key safety number ${change.safetyNumber.join(" ")}`}>
              {change.safetyNumber.map((group, i) => (
                <span key={i} className={styles.group}>
                  {group}
                </span>
              ))}
            </div>
            <button type="button" className={styles.primary} disabled={trusting} onClick={() => void trust()}>
              Trust the new keys
            </button>
          </>
        )}
      </>
    );
  }
  if (!groups) return null;
  return (
    <>
      <p className={styles.lead}>Device keys (preview): this number covers your new per-device keys. Compare it the same way.</p>
      <div className={styles.digits} role="img" aria-label={`Device-key safety number ${groups.join(" ")}`}>
        {groups.map((group, i) => (
          <span key={i} className={styles.group}>
            {group}
          </span>
        ))}
      </div>
    </>
  );
}

import sodium from "libsodium-wrappers-sumo";
import { useCallback, useEffect, useRef, useState } from "react";
import { MonitorIcon } from "../../components/icons24.js";
import * as api from "../../lib/api.js";
import { decryptWithConversationKey, encryptWithConversationKey, fromBase64, ready, toBase64 } from "../../lib/e2eCrypto.js";
import { useAppState } from "../../state/AppStateContext.js";
import { DeviceKeys } from "./DeviceKeys.js";
import styles from "./SettingsPages.module.css";

/** Device names are encrypted with a key only this account's devices can derive; the server sees ciphertext. */
function labelKey(privateKey: Uint8Array): Uint8Array {
  return sodium.crypto_generichash(32, "redvault-device-label-v1", privateKey);
}

/** Until you name it: this app never reads the browser's identifying details (see test/noTracking.test.ts). */
const DEFAULT_NAME = "Web browser";
/** Other devices shown before "Show all". */
const COLLAPSED_COUNT = 5;

interface DeviceRow extends api.SessionDto {
  name: string | null;
}

/**
 * Every device signed in to this account. The server knows nothing about
 * them but an id and the day the sign-in expires (no IP, browser or login
 * time), so the names come from your own devices, encrypted.
 */
export function LinkedDevices() {
  const { state } = useAppState();
  const identity = state.identity;
  const [rows, setRows] = useState<DeviceRow[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [editing, setEditing] = useState(false);
  const [name, setName] = useState("");
  const [showAll, setShowAll] = useState(false);

  const load = useCallback(async () => {
    if (!identity) return;
    try {
      await ready();
      const key = labelKey(identity.privateKey);
      const sessions = await api.listSessions();
      setRows(
        sessions.map((s) => {
          let label: string | null = null;
          try {
            label = s.label ? decryptWithConversationKey(fromBase64(s.label.ciphertext), fromBase64(s.label.nonce), key) : null;
          } catch {
            label = null;
          }
          return { ...s, name: label };
        })
      );
      setError(null);
    } catch {
      setError("Couldn't load your devices. Try again.");
    }
  }, [identity]);

  useEffect(() => {
    void load();
  }, [load]);

  // Name this device the first time it's seen here (once: a failed save isn't retried in a loop).
  const current = rows?.find((r) => r.current);
  const namedOnce = useRef(false);
  useEffect(() => {
    if (!identity || !current || current.name || namedOnce.current) return;
    namedOnce.current = true;
    const { ciphertext, nonce } = encryptWithConversationKey(DEFAULT_NAME, labelKey(identity.privateKey));
    api
      .setSessionLabel({ ciphertext: toBase64(ciphertext), nonce: toBase64(nonce) })
      .then(load)
      .catch(() => {});
  }, [identity, current, load]);

  async function saveName() {
    if (!identity || !name.trim()) return;
    const { ciphertext, nonce } = encryptWithConversationKey(name.trim().slice(0, 60), labelKey(identity.privateKey));
    try {
      await api.setSessionLabel({ ciphertext: toBase64(ciphertext), nonce: toBase64(nonce) });
      setEditing(false);
      await load();
    } catch {
      setError("Couldn't rename this device.");
    }
  }

  async function signOutOthers() {
    const others = rows?.filter((r) => !r.current).length ?? 0;
    if (!window.confirm(`Sign out ${others} other ${others === 1 ? "device" : "devices"}? Each will need your password to sign in again.`)) return;
    try {
      await api.signOutOtherSessions();
      setShowAll(false);
      await load();
    } catch {
      setError("Couldn't sign the other devices out. Try again.");
    }
  }

  // This device first, then the rest (newest sign-ins first, as the server sends them).
  const ordered = rows ? [...rows.filter((r) => r.current), ...rows.filter((r) => !r.current)] : null;
  const otherCount = rows ? rows.filter((r) => !r.current).length : 0;
  const visible = ordered && !showAll ? ordered.slice(0, COLLAPSED_COUNT + 1) : ordered;

  async function signOut(row: DeviceRow) {
    if (!window.confirm(`Sign out ${row.name ?? "this device"}? It will need your password to sign in again.`)) return;
    try {
      await api.signOutSession(row.id);
      await load();
    } catch {
      setError("Couldn't sign that device out. Try again.");
    }
  }

  return (
    <>
      <p className={styles.lead}>
        Everywhere you're signed in. The server keeps no IP address, browser or sign-in time for any of them: only when each sign-in
        expires. Device names are encrypted by your own devices.
      </p>
      {error && <p className={styles.error}>{error}</p>}
      <div className={styles.group}>
        {rows === null && !error && (
          <div className={styles.row}>
            <span className={styles.rowDetail}>Loading…</span>
          </div>
        )}
        {visible?.map((row) => (
          <div key={row.id} className={styles.row}>
            <span className={styles.person}>
              <MonitorIcon size={20} />
              {row.current && editing ? (
                <input
                  className={styles.labelInput}
                  value={name}
                  autoFocus
                  maxLength={60}
                  aria-label="Name this device"
                  onChange={(e) => setName(e.target.value)}
                  onKeyDown={(e) => {
                    if (e.key === "Enter") void saveName();
                    if (e.key === "Escape") setEditing(false);
                  }}
                />
              ) : (
                <span className={styles.rowText}>
                  <span className={styles.rowTitle}>
                    {row.name ?? "Unnamed device"} {row.current && <span className={styles.badge}>This device</span>}
                  </span>
                  {row.awaitingTwoFactor ? (
                    <span className={styles.warn}>
                      Password entered, two-factor code not yet. If this wasn't you, sign it out and change your password.
                    </span>
                  ) : (
                    <span className={styles.rowDetail}>
                      Signed in until {new Date(`${row.expiresOn}T00:00:00`).toLocaleDateString(undefined, { month: "long", day: "numeric", year: "numeric" })}
                    </span>
                  )}
                </span>
              )}
            </span>
            {row.current ? (
              editing ? (
                <button type="button" className={styles.smallButton} onClick={saveName} disabled={!name.trim()}>
                  Save
                </button>
              ) : (
                <button
                  type="button"
                  className={styles.smallButton}
                  onClick={() => {
                    setName(row.name ?? "");
                    setEditing(true);
                  }}
                >
                  Rename
                </button>
              )
            ) : (
              <button type="button" className={styles.dangerButton} onClick={() => signOut(row)}>
                Sign out
              </button>
            )}
          </div>
        ))}
        {ordered && visible && ordered.length > visible.length && (
          <div className={styles.row}>
            <button type="button" className={styles.smallButton} onClick={() => setShowAll(true)}>
              Show all {ordered.length} devices
            </button>
          </div>
        )}
      </div>
      {otherCount > 0 && (
        <div>
          <button type="button" className={styles.dangerButton} onClick={signOutOthers}>
            Sign out all other devices
          </button>
        </div>
      )}
      <DeviceKeys />
    </>
  );
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useCallback, useEffect, useState } from "react";
import * as api from "../../lib/api.js";
import {
  createRecoveryBackup,
  EngineError,
  restoreWithRecoveryKey,
  revokeDevice,
  setUpFirstDevice,
  startNewDevice,
  V3Device,
} from "../../lib/e2eV3/device.js";
import { deviceStorageAvailable } from "../../lib/e2eV3/deviceStore.js";
import { engineIncluded } from "../../lib/e2eV3/engine.js";
import { NewDevicePairing, TrustedPairing } from "../../lib/e2eV3/pairing.js";
import { useAppState } from "../../state/AppStateContext.js";
import styles from "./SettingsPages.module.css";

/** What each security event means to the person reading it. */
const EVENT_TEXT: Record<string, string> = {
  device_registered: "A device registered its keys (trusted only once approved)",
  device_list_created: "Device keys were set up for this account",
  device_added: "A device was approved",
  device_removed: "A device was removed",
  device_unregistered: "A device's keys were deleted",
  enrollment_requested: "A new device asked to pair",
  account_key_rotated: "The account key was replaced by a trusted device",
  account_reset_requested: "Someone asked to reset this account's keys",
  account_reset_cancelled: "A reset of this account's keys was cancelled",
  account_reset_completed: "This account's keys were reset",
  recovery_backup_updated: "A recovery backup was saved",
  recovery_backup_downloaded: "The recovery backup was downloaded",
};

interface Status {
  device: V3Device | null;
  /** This browser's device holds the account's list (false while it waits to be linked). */
  linked: boolean;
  list: api.V3SignedDeviceList | null;
  pendingReset: api.V3ListView["pendingReset"];
  trusted: boolean;
  listedHere: boolean;
  registered: api.V3OwnDevice[];
  enrollments: { id: string; newDeviceId: string }[];
  events: api.V3Event[];
}

function message(err: unknown): string {
  if (err instanceof EngineError || err instanceof api.ApiError) return err.message;
  return "Something went wrong. Try again.";
}

function Codes({ codes }: { codes: number[] }) {
  return (
    <span className={styles.mono} aria-label="Pairing codes">
      {codes.join("  ")}
    </span>
  );
}

/**
 * Per-device encryption keys (protocol 3): a development preview behind the
 * server's E2E_V3_ENABLED flag. Messages keep using the current encryption.
 */
export function DeviceKeys() {
  const { state } = useAppState();
  const identity = state.identity;
  // Effects key on the id, not the identity object (which may be a new object on every render).
  const userId = identity?.userId ?? null;
  const [enabled, setEnabled] = useState(false);
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [recoveryKey, setRecoveryKey] = useState<string | null>(null);
  const [recoveryInput, setRecoveryInput] = useState("");
  const [answering, setAnswering] = useState<{ pairing: TrustedPairing; codes: number[] } | null>(null);
  const [joining, setJoining] = useState<{ pairing: NewDevicePairing; codes: number[] | null } | null>(null);

  const load = useCallback(async () => {
    if (!userId) return;
    try {
      const device = await V3Device.open(userId);
      const view = await api.getV3DeviceList(userId);
      let trusted = false;
      let listedHere = false;
      let linked = false;
      let list = view.deviceList;
      const ownBefore = device ? await device.op("deviceList") : null;
      linked = Boolean(ownBefore?.ok);
      if (device && linked && list) {
        const checked = await device.op("verifyDeviceList", { deviceList: list });
        if (!checked.ok) throw new EngineError(checked.error ?? "bad_device_list", "This account's device list doesn't verify. Don't approve anything until you know why.");
        listedHere = checked.thisDeviceListed !== false;
        const own = await device.op("deviceList");
        trusted = Boolean(own.ok && own.trusted);
        list = (own.deviceList as api.V3SignedDeviceList | undefined) ?? list;
      }
      const [registered, enrollments, events] = await Promise.all([
        api.listV3Devices(),
        device && trusted ? api.listV3Enrollments() : Promise.resolve([]),
        api.getV3Events(),
      ]);
      setStatus({ device, linked, list, pendingReset: view.pendingReset, trusted, listedHere, registered, enrollments, events: events.slice(-8).reverse() });
      setError(null);
    } catch (err) {
      setError(message(err));
    }
  }, [userId]);

  useEffect(() => {
    if (!userId || !engineIncluded() || !deviceStorageAvailable()) return;
    Promise.resolve()
      .then(() => api.getV3Config())
      .then((config) => {
        setEnabled(config.enabled);
        if (config.enabled) void load();
      })
      .catch(() => setEnabled(false));
  }, [userId, load]);

  if (!enabled || !identity) return null;

  async function run(action: () => Promise<void>) {
    setBusy(true);
    setError(null);
    try {
      await action();
      await load();
    } catch (err) {
      setError(message(err));
    } finally {
      setBusy(false);
    }
  }

  const device = status?.device ?? null;
  const signed = status?.list?.devices ?? [];
  const waiting = status?.registered.filter((d) => !d.listed && d.deviceId !== device?.deviceId) ?? [];

  return (
    <>
      <h3 className={styles.subhead}>Device keys (preview)</h3>
      <p className={styles.lead}>
        In development: each device gets its own encryption keys, and a new device is trusted only when one of your trusted devices
        approves it. Your messages still use the current encryption.
      </p>
      {error && <p className={styles.error}>{error}</p>}

      {status && !device && !status.list && (
        <div className={styles.group}>
          <div className={styles.row}>
            <span className={styles.rowText}>
              <span className={styles.rowTitle}>Set up device keys on this browser</span>
              <span className={styles.rowDetail}>This browser becomes your first trusted device. Your current key vouches for the new one.</span>
            </span>
            <button type="button" className={styles.smallButton} disabled={busy} onClick={() => run(() => setUpFirstDevice(identity.userId, identity.privateKey).then(() => {}))}>
              Set up
            </button>
          </div>
        </div>
      )}

      {status && (!device || !status.linked) && status.list && (
        <>
          {device && !joining && (
            <div className={styles.group}>
              <div className={styles.row}>
                <span className={styles.rowText}>
                  <span className={styles.rowTitle}>Pairing was interrupted</span>
                  <span className={styles.rowDetail}>This browser has keys but no trusted device has approved them yet. Start again.</span>
                </span>
                <button
                  type="button"
                  className={styles.smallButton}
                  disabled={busy}
                  onClick={() =>
                    run(async () => {
                      await device.op("cancelLink");
                      await device.forget();
                    })
                  }
                >
                  Start over
                </button>
              </div>
            </div>
          )}
        <div className={styles.group}>
          <div className={styles.row}>
            <span className={styles.rowText}>
              <span className={styles.rowTitle}>Pair this browser with a trusted device</span>
              <span className={styles.rowDetail}>
                Your password alone can't make this browser trusted. Open Linked devices on a trusted device and compare the codes.
              </span>
              {joining?.codes && (
                <>
                  <Codes codes={joining.codes} />
                  <span className={styles.rowDetail}>Do these match the numbers on your trusted device?</span>
                </>
              )}
              {joining && !joining.codes && <span className={styles.rowDetail}>Waiting for a trusted device to answer…</span>}
            </span>
            {!joining && !device && (
              <button
                type="button"
                className={styles.smallButton}
                disabled={busy}
                onClick={() =>
                  run(async () => {
                    const fresh = await startNewDevice(identity.userId);
                    const pairing = await NewDevicePairing.begin(fresh);
                    setJoining({ pairing, codes: null });
                    const codes = await pairing.codes();
                    setJoining({ pairing, codes });
                  })
                }
              >
                Start pairing
              </button>
            )}
            {joining?.codes && (
              <span className={styles.person}>
                <button
                  type="button"
                  className={styles.smallButton}
                  disabled={busy}
                  onClick={() =>
                    run(async () => {
                      await joining.pairing.confirm();
                      setJoining(null);
                    })
                  }
                >
                  They match
                </button>
                <button
                  type="button"
                  className={styles.dangerButton}
                  disabled={busy}
                  onClick={() =>
                    run(async () => {
                      await joining.pairing.cancel();
                      await joining.pairing.device.forget();
                      setJoining(null);
                    })
                  }
                >
                  They don't
                </button>
              </span>
            )}
          </div>
          <div className={styles.row}>
            <span className={styles.rowText}>
              <span className={styles.rowTitle}>No trusted device left? Use your recovery key</span>
              <input
                className={styles.labelInput}
                aria-label="Recovery key"
                placeholder="XXXX-XXXX-…"
                value={recoveryInput}
                onChange={(e) => setRecoveryInput(e.target.value)}
              />
            </span>
            <button
              type="button"
              className={styles.smallButton}
              disabled={busy || !recoveryInput.trim()}
              onClick={() =>
                run(async () => {
                  await restoreWithRecoveryKey(identity.userId, recoveryInput);
                  setRecoveryInput("");
                })
              }
            >
              Restore
            </button>
          </div>
        </div>
        </>
      )}

      {status?.pendingReset && (
        <div className={styles.group}>
          <div className={styles.row}>
            <span className={styles.rowText}>
              <span className={styles.warn}>
                Someone asked to reset this account's keys. It takes effect on {new Date(status.pendingReset.effectiveAt).toLocaleString()} unless a
                trusted device cancels it. If it wasn't you, cancel it and change your password.
              </span>
            </span>
            {device && status.trusted && (
              <button
                type="button"
                className={styles.dangerButton}
                disabled={busy}
                onClick={() =>
                  run(async () => {
                    const refusal = await device.must("cancelReset", { pendingAccountKey: status.pendingReset!.accountKey });
                    await api.cancelV3Reset(refusal.signature as string);
                  })
                }
              >
                Cancel reset
              </button>
            )}
          </div>
        </div>
      )}

      {device && status?.linked && !status.listedHere && (
        <div className={styles.group}>
          <div className={styles.row}>
            <span className={styles.rowText}>
              <span className={styles.warn}>This browser was removed from your trusted devices.</span>
            </span>
            <button type="button" className={styles.dangerButton} disabled={busy} onClick={() => run(() => device.forget())}>
              Delete its keys
            </button>
          </div>
        </div>
      )}

      {device && status?.linked && status.listedHere && (
        <>
          <div className={styles.group}>
            {signed.map((d) => (
              <div key={d.deviceId} className={styles.row}>
                <span className={styles.rowText}>
                  <span className={styles.rowTitle}>
                    Trusted device {d.deviceId === device.deviceId && <span className={styles.badge}>This browser</span>}
                  </span>
                  <span className={`${styles.rowDetail} ${styles.mono}`}>{d.ed25519.slice(0, 16)}…</span>
                </span>
                {d.deviceId !== device.deviceId && status.trusted && (
                  <button type="button" className={styles.dangerButton} disabled={busy} onClick={() => run(() => revokeDevice(device, d.deviceId))}>
                    Remove
                  </button>
                )}
              </div>
            ))}
            {waiting.map((d) => (
              <div key={d.deviceId} className={styles.row}>
                <span className={styles.rowText}>
                  <span className={styles.rowTitle}>Signed in, not trusted</span>
                  <span className={styles.rowDetail}>
                    {status.enrollments.some((e) => e.newDeviceId === d.deviceId)
                      ? "Asking to pair. Compare the codes on both screens before approving."
                      : "If you don't recognize it, remove it and change your password."}
                  </span>
                  {answering?.pairing.newDevice.deviceId === d.deviceId && (
                    <>
                      <Codes codes={answering.codes} />
                      <span className={styles.rowDetail}>Do these match the numbers on the new device?</span>
                    </>
                  )}
                </span>
                <span className={styles.person}>
                  {status.trusted && !answering && status.enrollments.some((e) => e.newDeviceId === d.deviceId) && (
                    <button
                      type="button"
                      className={styles.smallButton}
                      disabled={busy}
                      onClick={() =>
                        run(async () => {
                          const request = status.enrollments.find((e) => e.newDeviceId === d.deviceId)!;
                          setAnswering(await TrustedPairing.begin(device, request.id));
                        })
                      }
                    >
                      Compare codes
                    </button>
                  )}
                  {answering?.pairing.newDevice.deviceId === d.deviceId ? (
                    <>
                      <button
                        type="button"
                        className={styles.smallButton}
                        disabled={busy}
                        onClick={() =>
                          run(async () => {
                            await answering.pairing.confirm();
                            setAnswering(null);
                          })
                        }
                      >
                        They match
                      </button>
                      <button
                        type="button"
                        className={styles.dangerButton}
                        disabled={busy}
                        onClick={() =>
                          run(async () => {
                            await answering.pairing.reject();
                            setAnswering(null);
                          })
                        }
                      >
                        They don't
                      </button>
                    </>
                  ) : (
                    <button type="button" className={styles.dangerButton} disabled={busy} onClick={() => run(() => api.removeV3Device(d.deviceId))}>
                      Remove
                    </button>
                  )}
                </span>
              </div>
            ))}
          </div>

          {status.trusted && (
            <div className={styles.group}>
              <div className={styles.row}>
                <span className={styles.rowText}>
                  <span className={styles.rowTitle}>Recovery key</span>
                  <span className={styles.rowDetail}>
                    For when you have no trusted device left. Write it down; it's shown once and never stored here or on the server.
                  </span>
                  {recoveryKey && <span className={styles.mono}>{recoveryKey}</span>}
                </span>
                <button
                  type="button"
                  className={styles.smallButton}
                  disabled={busy}
                  onClick={() =>
                    run(async () => {
                      setRecoveryKey(await createRecoveryBackup(device));
                    })
                  }
                >
                  {recoveryKey ? "Make a new one" : "Create"}
                </button>
              </div>
            </div>
          )}
        </>
      )}

      {status && status.events.length > 0 && (
        <div className={styles.group}>
          {status.events.map((e) => (
            <div key={e.id} className={styles.row}>
              <span className={styles.rowText}>
                <span className={styles.rowDetail}>{EVENT_TEXT[e.kind] ?? e.kind}</span>
              </span>
              <span className={styles.rowDetail}>{new Date(`${e.createdOn}T00:00:00`).toLocaleDateString()}</span>
            </div>
          ))}
        </div>
      )}
    </>
  );
}

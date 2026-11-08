// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QRCode from "qrcode";
import { useEffect, useRef, useState } from "react";
import { Avatar } from "../components/Avatar.js";
import { DeviceKeySafetyNumber } from "../components/DeviceKeySafetyNumber.js";
import { CheckIcon, CloseIcon, CopyIcon, KeyIcon, QrIcon, ShieldCheckIcon } from "../components/icons24.js";
import { QrScanner } from "../components/QrScanner.js";
import { useDialogFocus } from "../components/useDialogFocus.js";
import { toBase64 } from "../lib/e2eCrypto.js";
import { checkSafetyQr, safetyQrPayload } from "../lib/safetyQr.js";
import { acceptContactKey, setVerified } from "../lib/verifiedContacts.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./Verify.module.css";

/** The safety-number dialog: compare the 60 digits, or scan each other's code, then mark the contact verified. */
export function Verify({ contactId, origin }: { contactId: string; origin: "new-contact" | "thread" }) {
  const { state, dispatch, contactsById } = useAppState();
  const contact = contactsById.get(contactId);
  const [qrDataUrl, setQrDataUrl] = useState<string | null>(null);
  const [scanning, setScanning] = useState(false);
  const [scanError, setScanError] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);
  const dialogRef = useRef<HTMLDivElement>(null);
  const close = () => dispatch({ type: "DECLINE_VERIFY" });
  useDialogFocus(dialogRef, close, !!contact);

  const myKey = state.identity?.publicKey;
  const theirKey = contact?.publicKey;
  useEffect(() => {
    if (!myKey || !theirKey) return;
    let cancelled = false;
    QRCode.toDataURL(safetyQrPayload(myKey, theirKey), { margin: 1, width: 240, color: { dark: "#141413", light: "#ECEAE3" } })
      .then((url) => {
        if (!cancelled) setQrDataUrl(url);
      })
      .catch(() => {
        // The 60 digits still work without the code.
      });
    return () => {
      cancelled = true;
    };
  }, [myKey, theirKey]);

  if (!contact || !state.identity || !contact.safetyNumber) return null;
  const firstName = contact.handle.split(/[\s.·]/)[0];

  function markVerified() {
    if (state.identity && contact!.publicKey) {
      setVerified(state.identity.userId, toBase64(contact!.publicKey), true);
      if (contact!.userId) acceptContactKey(state.identity.userId, contact!.userId, toBase64(contact!.publicKey));
    }
    dispatch({ type: "MARK_VERIFIED" });
  }

  function onScanned(text: string) {
    if (!myKey || !theirKey) return;
    const result = checkSafetyQr(text, myKey, theirKey);
    if (result === "not-a-safety-code") return; // keep looking
    setScanning(false);
    if (result === "match") {
      markVerified();
    } else {
      setScanError(
        "That code doesn't match. Either it isn't this contact's phone, or one of your keys was swapped: don't treat this chat as verified."
      );
    }
  }

  async function copyNumber() {
    try {
      await navigator.clipboard.writeText(contact!.safetyNumber!.join(" "));
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
    } catch {
      // Clipboard blocked: the digits are on screen to read out anyway.
    }
  }

  return (
    <div className={styles.backdrop} onMouseDown={(e) => e.target === e.currentTarget && close()}>
      <div ref={dialogRef} className={styles.dialog} role="dialog" aria-modal="true" aria-labelledby="verify-title">
        <div className={styles.top}>
          <div className={styles.chips} aria-hidden="true">
            <Avatar avatarUrl={state.identity.avatarUrl} seed={state.identity.handle} size={44} />
            <span className={styles.keyChip}>
              <KeyIcon size={18} />
            </span>
            <Avatar avatarUrl={contact.avatarUrl} seed={contact.handle} size={44} />
          </div>
          <button type="button" className={styles.close} aria-label="Close" onClick={close}>
            <CloseIcon />
          </button>
        </div>

        <h2 id="verify-title" className={`${styles.title} type-display`}>
          Verify safety number with {firstName}
        </h2>
        {contact.keyChanged && (
          <p className={styles.warning}>
            {contact.handle}'s safety number changed. They may have reinstalled or recovered their account, or someone may be
            intercepting. Check it again before you trust this chat or a call with them.
          </p>
        )}
        <p className={styles.lead}>
          {origin === "new-contact"
            ? `Compare these numbers with ${firstName} in person or over a call you trust, or scan each other's code. If they match, no one is intercepting your chats or calls.`
            : `Compare these numbers with ${firstName} in person or over a call you trust, or let them scan your code with their phone. If they match, no one is intercepting your chats or calls.`}
        </p>

        <div className={styles.body}>
          {scanning ? (
            <div className={styles.scanner}>
              <QrScanner onResult={onScanned} onCancel={() => setScanning(false)} />
            </div>
          ) : (
            <div className={styles.qrTile}>
              {qrDataUrl ? (
                <img src={qrDataUrl} alt={`Your safety code for ${contact.handle}, for them to scan`} className={styles.qr} />
              ) : (
                <QrIcon size={40} />
              )}
            </div>
          )}
          <div className={styles.digits} role="img" aria-label={`Safety number ${contact.safetyNumber.join(" ")}`}>
            {contact.safetyNumber.map((group, i) => (
              <span key={i} className={styles.group}>
                {group}
              </span>
            ))}
          </div>
        </div>

        {scanError && (
          <p className={styles.warning} role="alert">
            {scanError}
          </p>
        )}

        {contact.userId && <DeviceKeySafetyNumber userId={state.identity.userId} contactUserId={contact.userId} />}

        <div className={styles.actions}>
          {!scanning && (
            <button
              type="button"
              className={styles.secondary}
              onClick={() => {
                setScanError(null);
                setScanning(true);
              }}
            >
              <QrIcon size={18} /> Scan their code
            </button>
          )}
          <button type="button" className={styles.secondary} onClick={copyNumber}>
            {copied ? <CheckIcon size={18} /> : <CopyIcon size={18} />} {copied ? "Copied" : "Copy number"}
          </button>
          <button type="button" className={styles.primary} onClick={markVerified}>
            <ShieldCheckIcon size={18} /> Mark as verified
          </button>
        </div>
      </div>
    </div>
  );
}

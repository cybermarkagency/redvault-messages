// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Sessions between this device and each other device (plain data, no I/O).
//
// Each remote device has a SessionRecord: the session in use plus a few
// earlier ones. Keeping earlier ones matters when two devices start a
// session with each other at the same moment: each side ends up holding
// the other's session, and messages keep decrypting while they settle on one.
import {
  initiate,
  respond,
  type OneTimePreKey,
  type PqPreKey,
  type PreKeyBundle,
  type SignedPreKey,
} from "./pqxdh.js";
import { equalBytes, type X25519KeyPair } from "./primitives.js";
import {
  decodeWireMessage,
  encodeWireMessage,
  initInitiator,
  initResponder,
  ratchetDecrypt,
  ratchetEncrypt,
  RatchetError,
  type SessionState,
} from "./ratchet.js";

const MAX_PREVIOUS_SESSIONS = 4;

/** This device's own keys. Private halves never leave the browser. */
export interface LocalDevice {
  deviceId: string;
  /** Newest first. Older ones are kept a while for sessions other devices started with them. */
  signedPreKeys: SignedPreKey[];
  pqPreKeys: PqPreKey[];
  oneTimePreKeys: OneTimePreKey[];
  nextKeyId: number;
}

export interface SessionRecord {
  /** The remote account's identity key when the session started. A different key later means a new session. */
  remoteIdentity: Uint8Array;
  current: SessionState | null;
  previous: SessionState[];
}

export function startSession(identity: X25519KeyPair, bundle: PreKeyBundle, ourDeviceId: string, theirDeviceId: string): SessionRecord {
  const { sharedKey, associatedData, header } = initiate(identity, bundle, ourDeviceId, theirDeviceId);
  return {
    remoteIdentity: bundle.identityKey,
    current: initInitiator(sharedKey, bundle.signedPreKey.publicKey, associatedData, header),
    previous: [],
  };
}

export function encryptFor(record: SessionRecord, plaintext: Uint8Array): { record: SessionRecord; payload: Uint8Array } {
  if (!record.current) throw new RatchetError("No session");
  const { state, message } = ratchetEncrypt(record.current, plaintext);
  return { record: { ...record, current: state }, payload: encodeWireMessage(message) };
}

function promote(record: SessionRecord, index: number, state: SessionState): SessionRecord {
  if (index === 0) return { ...record, current: state };
  const previous = record.previous.filter((_, i) => i !== index - 1);
  const demoted = record.current ? [record.current] : [];
  return { ...record, current: state, previous: [...demoted, ...previous].slice(0, MAX_PREVIOUS_SESSIONS) };
}

export interface Decrypted {
  record: SessionRecord;
  plaintext: Uint8Array;
  /** Set when this message used up one of our one-time prekeys: delete its private half. */
  usedOneTimePreKeyId: number | null;
}

/**
 * Decrypts one payload from `senderDeviceId`. `senderIdentity` is the
 * sender account's public key as this app knows it: a prekey message
 * claiming any other identity is refused.
 */
export function decryptFrom(
  identity: X25519KeyPair,
  device: LocalDevice,
  existing: SessionRecord | undefined,
  payload: Uint8Array,
  senderDeviceId: string,
  senderIdentity: Uint8Array
): Decrypted {
  const message = decodeWireMessage(payload);
  const sessions = existing ? [existing.current, ...existing.previous] : [];

  if (message.preKey) {
    if (!equalBytes(message.preKey.identityKey, senderIdentity)) {
      throw new RatchetError("Sender's identity key doesn't match their account");
    }
    // A resend of the header on a session we already have.
    const index = sessions.findIndex((s) => s && equalBytes(s.baseKey, message.preKey!.ephemeralKey));
    if (index >= 0 && existing) {
      const { state, plaintext } = ratchetDecrypt(sessions[index]!, message);
      return { record: promote(existing, index, state), plaintext, usedOneTimePreKeyId: null };
    }

    const signedPreKey = device.signedPreKeys.find((k) => k.id === message.preKey!.signedPreKeyId);
    const pqPreKey = device.pqPreKeys.find((k) => k.id === message.preKey!.pqPreKeyId);
    const oneTimeId = message.preKey.oneTimePreKeyId;
    const oneTimePreKey = oneTimeId === null ? null : device.oneTimePreKeys.find((k) => k.id === oneTimeId);
    if (!signedPreKey || !pqPreKey || oneTimePreKey === undefined) {
      throw new RatchetError("The prekeys this message was sent to are gone");
    }
    const { sharedKey, associatedData } = respond(
      identity,
      signedPreKey,
      pqPreKey,
      oneTimePreKey,
      message.preKey,
      senderDeviceId,
      device.deviceId
    );
    const fresh = initResponder(sharedKey, signedPreKey.keyPair, associatedData, message.preKey.ephemeralKey);
    const { state, plaintext } = ratchetDecrypt(fresh, message);
    const kept = existing && equalBytes(existing.remoteIdentity, senderIdentity) ? sessions.filter((s): s is SessionState => s !== null) : [];
    return {
      record: { remoteIdentity: senderIdentity, current: state, previous: kept.slice(0, MAX_PREVIOUS_SESSIONS) },
      plaintext,
      usedOneTimePreKeyId: oneTimePreKey?.id ?? null,
    };
  }

  if (!existing || !equalBytes(existing.remoteIdentity, senderIdentity)) {
    throw new RatchetError("No session with this device");
  }
  for (let i = 0; i < sessions.length; i++) {
    const session = sessions[i];
    if (!session) continue;
    try {
      const { state, plaintext } = ratchetDecrypt(session, message);
      return { record: promote(existing, i, state), plaintext, usedOneTimePreKeyId: null };
    } catch {
      // Try the next one.
    }
  }
  throw new RatchetError("Message failed to decrypt");
}

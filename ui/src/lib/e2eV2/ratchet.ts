// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The Double Ratchet (https://signal.org/docs/specifications/doubleratchet/)
// for one pair of devices, and the wire format of the messages it produces.
// Every message gets a fresh key that's deleted once used, and each reply
// mixes in a new X25519 exchange, so a stolen key doesn't unlock earlier
// messages (forward secrecy) or, for long, later ones.
//
// State is plain data, so it can be stored as-is. ratchetDecrypt never
// changes the state it's given: it returns a new one only when a message
// decrypts, so a forged or corrupted message can't damage a session.
import {
  AEAD_NONCE_BYTES,
  aeadDecrypt,
  aeadEncrypt,
  concat,
  dh,
  equalBytes,
  generateX25519,
  hkdfSha256,
  hmacSha256,
  readU32,
  u32,
  type X25519KeyPair,
} from "./primitives.js";
import { KEM_CIPHERTEXT_BYTES, NO_ONE_TIME_PREKEY, type PreKeyHeader } from "./pqxdh.js";

/** Most message keys one chain may skip ahead by (messages lost or reordered). */
export const MAX_SKIP = 2000;
/** Most skipped message keys a session keeps; the oldest are dropped first. */
const MAX_STORED_SKIPPED = 2000;

const MESSAGE_TYPE_NORMAL = 1;
const MESSAGE_TYPE_PREKEY = 2;
const RATCHET_HEADER_BYTES = 32 + 4 + 4;
const PREKEY_HEADER_BYTES = 32 + 32 + 4 + 4 + 4 + KEM_CIPHERTEXT_BYTES;

export interface SkippedKey {
  ratchetKey: Uint8Array;
  n: number;
  messageKey: Uint8Array;
}

export interface SessionState {
  rootKey: Uint8Array;
  ourRatchet: X25519KeyPair;
  theirRatchetKey: Uint8Array | null;
  sendingChainKey: Uint8Array | null;
  receivingChainKey: Uint8Array | null;
  sendCount: number;
  receiveCount: number;
  previousSendCount: number;
  skipped: SkippedKey[];
  associatedData: Uint8Array;
  /** The PQXDH ephemeral key that started this session; tells a resent prekey message apart from a new session. */
  baseKey: Uint8Array;
  /** Initiator only: sent with every message until the other device replies. */
  pendingPreKey: PreKeyHeader | null;
}

export interface RatchetHeader {
  ratchetKey: Uint8Array;
  previousCount: number;
  n: number;
}

export interface WireMessage {
  preKey: PreKeyHeader | null;
  header: RatchetHeader;
  ciphertext: Uint8Array;
}

export class RatchetError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "RatchetError";
  }
}

function kdfRoot(rootKey: Uint8Array, dhOutput: Uint8Array): { rootKey: Uint8Array; chainKey: Uint8Array } {
  const out = hkdfSha256(dhOutput, rootKey, "RedvaultRatchet_v1", 64);
  return { rootKey: out.slice(0, 32), chainKey: out.slice(32) };
}

function kdfChain(chainKey: Uint8Array): { chainKey: Uint8Array; messageKey: Uint8Array } {
  return { messageKey: hmacSha256(chainKey, Uint8Array.of(1)), chainKey: hmacSha256(chainKey, Uint8Array.of(2)) };
}

function messageKeyMaterial(messageKey: Uint8Array): { key: Uint8Array; nonce: Uint8Array } {
  const out = hkdfSha256(messageKey, new Uint8Array(32), "RedvaultMessageKeys_v1", 32 + AEAD_NONCE_BYTES);
  return { key: out.slice(0, 32), nonce: out.slice(32) };
}

function encodeHeader(header: RatchetHeader): Uint8Array {
  return concat(header.ratchetKey, u32(header.previousCount), u32(header.n));
}

/** Initiator: starts a session from the PQXDH shared key and the responder's signed prekey. */
export function initInitiator(
  sharedKey: Uint8Array,
  theirSignedPreKey: Uint8Array,
  associatedData: Uint8Array,
  preKey: PreKeyHeader
): SessionState {
  const ourRatchet = generateX25519();
  const { rootKey, chainKey } = kdfRoot(sharedKey, dh(ourRatchet.privateKey, theirSignedPreKey));
  return {
    rootKey,
    ourRatchet,
    theirRatchetKey: theirSignedPreKey,
    sendingChainKey: chainKey,
    receivingChainKey: null,
    sendCount: 0,
    receiveCount: 0,
    previousSendCount: 0,
    skipped: [],
    associatedData,
    baseKey: preKey.ephemeralKey,
    pendingPreKey: preKey,
  };
}

/** Responder: starts a session from the PQXDH shared key, using its signed prekey as the first ratchet key. */
export function initResponder(
  sharedKey: Uint8Array,
  ourSignedPreKey: X25519KeyPair,
  associatedData: Uint8Array,
  baseKey: Uint8Array
): SessionState {
  return {
    rootKey: sharedKey,
    ourRatchet: ourSignedPreKey,
    theirRatchetKey: null,
    sendingChainKey: null,
    receivingChainKey: null,
    sendCount: 0,
    receiveCount: 0,
    previousSendCount: 0,
    skipped: [],
    associatedData,
    baseKey,
    pendingPreKey: null,
  };
}

function cloneState(state: SessionState): SessionState {
  return { ...state, skipped: [...state.skipped] };
}

export function ratchetEncrypt(state: SessionState, plaintext: Uint8Array): { state: SessionState; message: WireMessage } {
  if (!state.sendingChainKey) throw new RatchetError("This session can't send until the other device replies");
  const next = cloneState(state);
  const { chainKey, messageKey } = kdfChain(state.sendingChainKey);
  const header: RatchetHeader = { ratchetKey: state.ourRatchet.publicKey, previousCount: state.previousSendCount, n: state.sendCount };
  const { key, nonce } = messageKeyMaterial(messageKey);
  const ciphertext = aeadEncrypt(key, nonce, plaintext, concat(state.associatedData, encodeHeader(header)));
  next.sendingChainKey = chainKey;
  next.sendCount = state.sendCount + 1;
  return { state: next, message: { preKey: state.pendingPreKey, header, ciphertext } };
}

function skipMessageKeys(state: SessionState, until: number): void {
  if (!state.receivingChainKey || !state.theirRatchetKey) return;
  if (until - state.receiveCount > MAX_SKIP) throw new RatchetError("Too many skipped messages");
  let chainKey = state.receivingChainKey;
  while (state.receiveCount < until) {
    const step = kdfChain(chainKey);
    state.skipped.push({ ratchetKey: state.theirRatchetKey, n: state.receiveCount, messageKey: step.messageKey });
    chainKey = step.chainKey;
    state.receiveCount++;
  }
  state.receivingChainKey = chainKey;
  if (state.skipped.length > MAX_STORED_SKIPPED) state.skipped.splice(0, state.skipped.length - MAX_STORED_SKIPPED);
}

function decryptWith(state: SessionState, messageKey: Uint8Array, message: WireMessage): Uint8Array {
  const { key, nonce } = messageKeyMaterial(messageKey);
  try {
    return aeadDecrypt(key, nonce, message.ciphertext, concat(state.associatedData, encodeHeader(message.header)));
  } catch {
    throw new RatchetError("Message failed to decrypt");
  }
}

export function ratchetDecrypt(state: SessionState, message: WireMessage): { state: SessionState; plaintext: Uint8Array } {
  const next = cloneState(state);
  const { header } = message;

  const skippedIndex = next.skipped.findIndex((s) => s.n === header.n && equalBytes(s.ratchetKey, header.ratchetKey));
  if (skippedIndex >= 0) {
    const plaintext = decryptWith(next, next.skipped[skippedIndex].messageKey, message);
    next.skipped.splice(skippedIndex, 1);
    return { state: next, plaintext };
  }

  if (!next.theirRatchetKey || !equalBytes(header.ratchetKey, next.theirRatchetKey)) {
    skipMessageKeys(next, header.previousCount);
    // A new ratchet step from the other device.
    next.previousSendCount = next.sendCount;
    next.sendCount = 0;
    next.receiveCount = 0;
    next.theirRatchetKey = header.ratchetKey;
    const receiving = kdfRoot(next.rootKey, dh(next.ourRatchet.privateKey, header.ratchetKey));
    next.receivingChainKey = receiving.chainKey;
    next.ourRatchet = generateX25519();
    const sending = kdfRoot(receiving.rootKey, dh(next.ourRatchet.privateKey, header.ratchetKey));
    next.rootKey = sending.rootKey;
    next.sendingChainKey = sending.chainKey;
  }

  skipMessageKeys(next, header.n);
  const step = kdfChain(next.receivingChainKey!);
  const plaintext = decryptWith(next, step.messageKey, message);
  next.receivingChainKey = step.chainKey;
  next.receiveCount++;
  // Anything decrypted proves the other device has the session, so stop resending the prekey header.
  next.pendingPreKey = null;
  return { state: next, plaintext };
}

// --- Wire format ---

export function encodeWireMessage(message: WireMessage): Uint8Array {
  const body = concat(encodeHeader(message.header), message.ciphertext);
  if (!message.preKey) return concat(Uint8Array.of(MESSAGE_TYPE_NORMAL), body);
  const p = message.preKey;
  return concat(
    Uint8Array.of(MESSAGE_TYPE_PREKEY),
    p.identityKey,
    p.ephemeralKey,
    u32(p.signedPreKeyId),
    u32(p.pqPreKeyId),
    u32(p.oneTimePreKeyId ?? NO_ONE_TIME_PREKEY),
    p.kemCiphertext,
    body
  );
}

export function decodeWireMessage(bytes: Uint8Array): WireMessage {
  let offset = 1;
  let preKey: PreKeyHeader | null = null;
  if (bytes[0] === MESSAGE_TYPE_PREKEY) {
    if (bytes.length < 1 + PREKEY_HEADER_BYTES + RATCHET_HEADER_BYTES) throw new RatchetError("Message too short");
    const oneTimePreKeyId = readU32(bytes, offset + 72);
    preKey = {
      identityKey: bytes.slice(offset, offset + 32),
      ephemeralKey: bytes.slice(offset + 32, offset + 64),
      signedPreKeyId: readU32(bytes, offset + 64),
      pqPreKeyId: readU32(bytes, offset + 68),
      oneTimePreKeyId: oneTimePreKeyId === NO_ONE_TIME_PREKEY ? null : oneTimePreKeyId,
      kemCiphertext: bytes.slice(offset + 76, offset + 76 + KEM_CIPHERTEXT_BYTES),
    };
    offset += PREKEY_HEADER_BYTES;
  } else if (bytes[0] !== MESSAGE_TYPE_NORMAL) {
    throw new RatchetError("Unknown message type");
  }
  if (bytes.length < offset + RATCHET_HEADER_BYTES) throw new RatchetError("Message too short");
  return {
    preKey,
    header: {
      ratchetKey: bytes.slice(offset, offset + 32),
      previousCount: readU32(bytes, offset + 32),
      n: readU32(bytes, offset + 36),
    },
    ciphertext: bytes.slice(offset + RATCHET_HEADER_BYTES),
  };
}

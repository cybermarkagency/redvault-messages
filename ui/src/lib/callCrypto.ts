// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// End-to-end protection for call setup. The server relays offers and
// answers but must not be able to read, change or forge them: the SDP they
// carry includes each side's DTLS certificate fingerprint, and WebRTC only
// completes the encrypted (DTLS-SRTP) media connection with the peer
// holding that exact certificate. So whoever controls the SDP controls who
// you're really talking to.
//
// Each signal is sealed with crypto_box (X25519 + XSalsa20-Poly1305)
// between the sender's identity private key and the recipient's identity
// public key: the same keys the safety number fingerprints. Opening it
// proves it came from the holder of that contact's key and wasn't altered;
// a server that swapped keys in would change the safety number, which is
// what contact verification (and the key-change warning) catches.
//
// The payload also names the call, the conversation, both people and its
// own purpose, and is refused if any of those don't match or it's stale,
// so a sealed signal can't be replayed into another call or turned around.
import sodium from "libsodium-wrappers-sumo";
import type { CallKind, SealedSignalDto } from "./api.js";
import { fromBase64, toBase64 } from "./e2eCrypto.js";

/** "restart-offer" / "restart-answer": an ICE restart with fresh TURN credentials during a long call (lib/turnRenewal.ts). */
export type CallSignalType = "offer" | "answer" | "restart-offer" | "restart-answer";

export interface CallSignal {
  v: 1;
  type: CallSignalType;
  callId: string;
  conversationId: string;
  from: string;
  to: string;
  kind: CallKind;
  sdp: string;
  /** Epoch ms when sealed; signals older than MAX_SIGNAL_AGE_MS are refused. */
  sentAt: number;
}

/** Calls ring for 45s at most; anything much older is a replay. */
const MAX_SIGNAL_AGE_MS = 5 * 60 * 1000;

export class CallSignalError extends Error {}

/** Seals `signal` so only the holder of `recipientPublicKey` can open it, and they know it's from us. */
export function sealCallSignal(signal: CallSignal, recipientPublicKey: Uint8Array, senderPrivateKey: Uint8Array): SealedSignalDto {
  const nonce = sodium.randombytes_buf(sodium.crypto_box_NONCEBYTES);
  // A string, not pre-encoded bytes: see e2eCrypto.encryptWithConversationKey
  // for the libsodium type-check bug this sidesteps.
  const ciphertext = sodium.crypto_box_easy(JSON.stringify(signal), nonce, recipientPublicKey, senderPrivateKey);
  return { ciphertext: toBase64(ciphertext), nonce: toBase64(nonce) };
}

/**
 * Opens a sealed signal from `senderPublicKey` and checks it's exactly the
 * one expected. Throws CallSignalError otherwise; the caller must then
 * drop the call rather than connect.
 */
export function openCallSignal(
  sealed: SealedSignalDto,
  senderPublicKey: Uint8Array,
  recipientPrivateKey: Uint8Array,
  expected: { type: CallSignalType; callId: string; conversationId: string; from: string; to: string; kind?: CallKind },
  now = Date.now()
): CallSignal {
  let plaintext: string;
  try {
    const opened = sodium.crypto_box_open_easy(fromBase64(sealed.ciphertext), fromBase64(sealed.nonce), senderPublicKey, recipientPrivateKey);
    plaintext = sodium.to_string(opened);
  } catch {
    throw new CallSignalError("This call couldn't be verified as coming from your contact's key.");
  }
  let signal: CallSignal;
  try {
    signal = JSON.parse(plaintext) as CallSignal;
  } catch {
    throw new CallSignalError("Malformed call setup.");
  }
  const matches =
    signal.v === 1 &&
    signal.type === expected.type &&
    signal.callId === expected.callId &&
    signal.conversationId === expected.conversationId &&
    signal.from === expected.from &&
    signal.to === expected.to &&
    (expected.kind === undefined || signal.kind === expected.kind) &&
    (signal.kind === "voice" || signal.kind === "video") &&
    typeof signal.sdp === "string";
  if (!matches) throw new CallSignalError("Call setup didn't match this call.");
  if (typeof signal.sentAt !== "number" || Math.abs(now - signal.sentAt) > MAX_SIGNAL_AGE_MS) {
    throw new CallSignalError("Call setup is too old.");
  }
  if (!/^a=fingerprint:/m.test(signal.sdp)) {
    // Without a DTLS fingerprint there'd be nothing binding the media to this key.
    throw new CallSignalError("Call setup is missing its encryption fingerprint.");
  }
  return signal;
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// PQXDH key agreement (https://signal.org/docs/specifications/pqxdh/), with
// X25519, SHA-256 and ML-KEM-768. The identity key is the account's
// existing X25519 key, shared by all of the account's devices; each device
// publishes its own signed prekey, signed ML-KEM prekey and one-time prekeys.
// Byte layouts are in docs/e2e-v2-protocol.md.
import { ml_kem768 } from "@noble/post-quantum/ml-kem.js";
import {
  concat,
  dh,
  generateX25519,
  hkdfSha256,
  u32,
  utf8,
  uuidBytes,
  xeddsaSign,
  xeddsaVerify,
  type X25519KeyPair,
} from "./primitives.js";

export const KEM_PUBLIC_KEY_BYTES = 1184;
export const KEM_CIPHERTEXT_BYTES = 1088;
/** On the wire, in place of a one-time prekey id, when the bundle had none left. */
export const NO_ONE_TIME_PREKEY = 0xffffffff;

const PQXDH_INFO = "RedvaultPQXDH_X25519_SHA-256_ML-KEM-768";

export interface SignedPreKey {
  id: number;
  keyPair: X25519KeyPair;
  signature: Uint8Array;
  createdAt: number;
}

export interface PqPreKey {
  id: number;
  publicKey: Uint8Array;
  secretKey: Uint8Array;
  signature: Uint8Array;
  createdAt: number;
}

export interface OneTimePreKey {
  id: number;
  keyPair: X25519KeyPair;
}

/** One device's public keys, as the server hands them out. */
export interface PreKeyBundle {
  identityKey: Uint8Array;
  signedPreKey: { id: number; publicKey: Uint8Array; signature: Uint8Array };
  pqPreKey: { id: number; publicKey: Uint8Array; signature: Uint8Array };
  oneTimePreKey: { id: number; publicKey: Uint8Array } | null;
}

/** Sent with an initiator's messages until the first reply arrives. */
export interface PreKeyHeader {
  identityKey: Uint8Array;
  ephemeralKey: Uint8Array;
  signedPreKeyId: number;
  pqPreKeyId: number;
  oneTimePreKeyId: number | null;
  kemCiphertext: Uint8Array;
}

function signedPreKeyMessage(id: number, publicKey: Uint8Array): Uint8Array {
  return concat(utf8("RedvaultSignedPreKey_v1"), u32(id), publicKey);
}

function pqPreKeyMessage(id: number, publicKey: Uint8Array): Uint8Array {
  return concat(utf8("RedvaultPqPreKey_v1"), u32(id), publicKey);
}

export function generateSignedPreKey(identity: X25519KeyPair, id: number): SignedPreKey {
  const keyPair = generateX25519();
  return { id, keyPair, signature: xeddsaSign(identity.privateKey, signedPreKeyMessage(id, keyPair.publicKey)), createdAt: Date.now() };
}

export function generatePqPreKey(identity: X25519KeyPair, id: number): PqPreKey {
  const { publicKey, secretKey } = ml_kem768.keygen();
  return {
    id,
    publicKey: Uint8Array.from(publicKey),
    secretKey: Uint8Array.from(secretKey),
    signature: xeddsaSign(identity.privateKey, pqPreKeyMessage(id, publicKey)),
    createdAt: Date.now(),
  };
}

export function generateOneTimePreKeys(firstId: number, count: number): OneTimePreKey[] {
  return Array.from({ length: count }, (_, i) => ({ id: firstId + i, keyPair: generateX25519() }));
}

/** Associated data bound into every message of the session: both identity keys and both device ids, initiator first. */
export function sessionAssociatedData(
  initiatorIdentity: Uint8Array,
  responderIdentity: Uint8Array,
  initiatorDeviceId: string,
  responderDeviceId: string
): Uint8Array {
  return concat(initiatorIdentity, responderIdentity, uuidBytes(initiatorDeviceId), uuidBytes(responderDeviceId));
}

function deriveSharedKey(dhOutputs: Uint8Array[], kemSecret: Uint8Array): Uint8Array {
  const ikm = concat(new Uint8Array(32).fill(0xff), ...dhOutputs, kemSecret);
  return hkdfSha256(ikm, new Uint8Array(32), PQXDH_INFO, 32);
}

export class BundleVerificationError extends Error {
  constructor() {
    super("A device's prekeys aren't signed by its account's identity key");
    this.name = "BundleVerificationError";
  }
}

/** Initiator side: checks the bundle's signatures and computes the session's shared key. */
export function initiate(
  identity: X25519KeyPair,
  bundle: PreKeyBundle,
  ourDeviceId: string,
  theirDeviceId: string
): { sharedKey: Uint8Array; associatedData: Uint8Array; header: PreKeyHeader } {
  const { signedPreKey, pqPreKey, oneTimePreKey } = bundle;
  if (
    !xeddsaVerify(bundle.identityKey, signedPreKeyMessage(signedPreKey.id, signedPreKey.publicKey), signedPreKey.signature) ||
    !xeddsaVerify(bundle.identityKey, pqPreKeyMessage(pqPreKey.id, pqPreKey.publicKey), pqPreKey.signature) ||
    pqPreKey.publicKey.length !== KEM_PUBLIC_KEY_BYTES
  ) {
    throw new BundleVerificationError();
  }

  const ephemeral = generateX25519();
  const dhOutputs = [
    dh(identity.privateKey, signedPreKey.publicKey),
    dh(ephemeral.privateKey, bundle.identityKey),
    dh(ephemeral.privateKey, signedPreKey.publicKey),
  ];
  if (oneTimePreKey) dhOutputs.push(dh(ephemeral.privateKey, oneTimePreKey.publicKey));
  const { cipherText, sharedSecret } = ml_kem768.encapsulate(pqPreKey.publicKey);

  return {
    sharedKey: deriveSharedKey(dhOutputs, sharedSecret),
    associatedData: sessionAssociatedData(identity.publicKey, bundle.identityKey, ourDeviceId, theirDeviceId),
    header: {
      identityKey: identity.publicKey,
      ephemeralKey: ephemeral.publicKey,
      signedPreKeyId: signedPreKey.id,
      pqPreKeyId: pqPreKey.id,
      oneTimePreKeyId: oneTimePreKey?.id ?? null,
      kemCiphertext: Uint8Array.from(cipherText),
    },
  };
}

/** Responder side: recomputes the shared key from the initiator's header and our own prekeys. */
export function respond(
  identity: X25519KeyPair,
  signedPreKey: SignedPreKey,
  pqPreKey: PqPreKey,
  oneTimePreKey: OneTimePreKey | null,
  header: PreKeyHeader,
  theirDeviceId: string,
  ourDeviceId: string
): { sharedKey: Uint8Array; associatedData: Uint8Array } {
  const dhOutputs = [
    dh(signedPreKey.keyPair.privateKey, header.identityKey),
    dh(identity.privateKey, header.ephemeralKey),
    dh(signedPreKey.keyPair.privateKey, header.ephemeralKey),
  ];
  if (oneTimePreKey) dhOutputs.push(dh(oneTimePreKey.keyPair.privateKey, header.ephemeralKey));
  const kemSecret = ml_kem768.decapsulate(header.kemCiphertext, pqPreKey.secretKey);
  return {
    sharedKey: deriveSharedKey(dhOutputs, kemSecret),
    associatedData: sessionAssociatedData(header.identityKey, identity.publicKey, theirDeviceId, ourDeviceId),
  };
}

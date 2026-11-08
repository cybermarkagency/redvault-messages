// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Checking the transparency log on this device:
//   1. the latest checkpoint's Ed25519 signature, under a log key pinned
//      the first time this browser saw it;
//   2. that it's an append-only extension of the last checkpoint this
//      browser verified (a consistency proof), so history can't be
//      rewritten or forked without it showing here;
//   3. that each of your own messages is in it (an inclusion proof for the
//      commitment this device computed itself).
import sodium from "libsodium-wrappers-sumo";
import * as api from "./api.js";
import { fromBase64 } from "./e2eCrypto.js";
import { bytesToHex, hexToBytes, leafHash, verifyConsistency, verifyInclusion } from "./merkle.js";

const PINNED_KEY = "redvaultsystem:log:public-key";
const LAST_SEEN = "redvaultsystem:log:last-checkpoint";

export type KeyStatus = "pinned" | "first-use" | "changed";

export interface VerifiedCheckpoint extends api.LogCheckpointDto {
  keyStatus: KeyStatus;
  /** Consistent with the last checkpoint this browser verified; null when there was none to compare with. */
  consistent: boolean | null;
}

export class LogVerificationError extends Error {}

/** Exactly what the server signs (server/src/services/transparencyLogService.ts's checkpointMessage). */
export function checkpointMessage(treeSize: number, rootHashHex: string, signedAtIso: string): string {
  return `redvault-transparency-log-v1\n${treeSize}\n${rootHashHex}\n${signedAtIso}`;
}

export function signatureValid(checkpoint: api.LogCheckpointDto): boolean {
  try {
    return sodium.crypto_sign_verify_detached(
      fromBase64(checkpoint.signature),
      checkpointMessage(checkpoint.treeSize, checkpoint.rootHash, checkpoint.signedAt),
      fromBase64(checkpoint.publicKey)
    );
  } catch {
    return false;
  }
}

/** A short, readable fingerprint of the log's signing key, to compare with what the operator publishes. */
export function keyFingerprint(publicKeyB64: string): string {
  const hex = bytesToHex(fromBase64(publicKeyB64));
  return hex.slice(0, 16).replace(/(.{4})(?!$)/g, "$1 ");
}

function readJson<T>(key: string): T | null {
  try {
    const raw = localStorage.getItem(key);
    return raw ? (JSON.parse(raw) as T) : null;
  } catch {
    return null;
  }
}

function writeJson(key: string, value: unknown): void {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    // Best effort.
  }
}

/** Fetches and checks the latest checkpoint. Null when the log has no entries yet. Throws LogVerificationError when it fails a check. */
export async function fetchVerifiedCheckpoint(): Promise<VerifiedCheckpoint | null> {
  await sodium.ready;
  const { checkpoint } = await api.getLogCheckpoint();
  if (!checkpoint) return null;
  if (!signatureValid(checkpoint)) throw new LogVerificationError("The latest checkpoint's signature doesn't check out.");

  const pinned = readJson<string>(PINNED_KEY);
  let keyStatus: KeyStatus = "pinned";
  if (!pinned) {
    writeJson(PINNED_KEY, checkpoint.publicKey);
    keyStatus = "first-use";
  } else if (pinned !== checkpoint.publicKey) {
    keyStatus = "changed";
  }

  let consistent: boolean | null = null;
  const last = readJson<{ treeSize: number; rootHash: string }>(LAST_SEEN);
  if (keyStatus !== "changed" && last) {
    if (last.treeSize === checkpoint.treeSize) consistent = last.rootHash === checkpoint.rootHash;
    else if (last.treeSize > checkpoint.treeSize) consistent = false; // the log shrank: rolled back or forked
    else {
      const { path } = await api.getLogConsistency(last.treeSize, checkpoint.treeSize);
      consistent = await verifyConsistency(
        last.treeSize,
        checkpoint.treeSize,
        hexToBytes(last.rootHash),
        hexToBytes(checkpoint.rootHash),
        path.map(hexToBytes)
      );
    }
  }
  if (keyStatus !== "changed" && consistent !== false) writeJson(LAST_SEEN, { treeSize: checkpoint.treeSize, rootHash: checkpoint.rootHash });
  return { ...checkpoint, keyStatus, consistent };
}

/** Trust the log's new signing key (after checking it with the operator). */
export function acceptLogKey(publicKeyB64: string): void {
  writeJson(PINNED_KEY, publicKeyB64);
  try {
    localStorage.removeItem(LAST_SEEN);
  } catch {
    // Best effort.
  }
}

/** Is the entry with this (locally computed) commitment at `index` in the checkpointed tree? */
export async function verifyEntryInclusion(commitmentHex: string, index: number, checkpoint: api.LogCheckpointDto): Promise<boolean> {
  if (index >= checkpoint.treeSize) return false;
  const proof = await api.getLogProof(index, checkpoint.treeSize);
  const leaf = await leafHash(hexToBytes(commitmentHex));
  if (bytesToHex(leaf) !== proof.leafHash) return false;
  return verifyInclusion(leaf, index, checkpoint.treeSize, proof.path.map(hexToBytes), hexToBytes(checkpoint.rootHash));
}

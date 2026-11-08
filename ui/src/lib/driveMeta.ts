// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Files ("drive") metadata, as the desktop app writes it
// (desktop/core/src/data/drive_repository.cpp, drive_crypto.cpp): a JSON
// object padded with spaces to a 128-byte bucket, sealed with a key derived
// from the account's private key. The server only ever sees the ciphertext
// and each file's encrypted size, so names, folders, types and dates are
// read here, on this device, and nowhere else.
import sodium from "libsodium-wrappers-sumo";
import type { BackupItemDto } from "./api.js";
import { decryptWithConversationKey, encryptWithConversationKey, fromBase64 } from "./e2eCrypto.js";

const PAD_BUCKET = 128;

export interface DriveEntry {
  id: string;
  kind: "file" | "folder";
  name: string;
  /** Parent folder id; "" at the top level. */
  parent: string;
  mime: string;
  /** Plaintext size in bytes (files). */
  size: number;
  /** What it takes up in storage (ciphertext); 0 for folders. */
  storedBytes: number;
  /** Epoch ms the file (this version) was uploaded, or the folder was last changed. */
  modified: number;
  /** Shared by every version of the same file. */
  fileId: string;
  version: number;
  trashed: boolean;
  trashedAt: number;
  /** The desktop's "PC backup" folder. */
  pcBackup: boolean;
  /** Share links made from this file (ids only). */
  linkIds: string[];
  /** Direct shares with people (user ids). */
  sharedWith: string[];
  /** Locked with its own key or password: its content key isn't in the metadata. */
  locked: boolean;
  /** Couldn't be decrypted on this device: counted by size only. */
  unreadable: boolean;
  /** The decrypted metadata as-is (in memory only), so a change can be re-sealed without losing fields this app doesn't use. */
  meta: Record<string, unknown> | null;
}

/** Same derivation as the desktop's crypto::drive::driveKey. */
export function driveKey(privateKey: Uint8Array): Uint8Array {
  return sodium.crypto_generichash(32, "redvault-drive-v1", privateKey);
}

export function sealDriveMeta(json: string, key: Uint8Array): { ciphertext: Uint8Array; nonce: Uint8Array } {
  const bytes = new TextEncoder().encode(json).length;
  const padded = json + " ".repeat((Math.floor(bytes / PAD_BUCKET) + 1) * PAD_BUCKET - bytes);
  return encryptWithConversationKey(padded, key);
}

export function openDriveMeta(ciphertext: Uint8Array, nonce: Uint8Array, key: Uint8Array): string {
  return decryptWithConversationKey(ciphertext, nonce, key).replace(/ +$/, "");
}

const num = (v: unknown, fallback = 0) => (typeof v === "number" && Number.isFinite(v) ? v : fallback);
const str = (v: unknown, fallback = "") => (typeof v === "string" ? v : fallback);

export function decodeDriveEntry(item: BackupItemDto, key: Uint8Array): DriveEntry {
  const base: DriveEntry = {
    id: item.id,
    kind: item.kind,
    name: item.kind === "folder" ? "Folder" : "Encrypted file",
    parent: "",
    mime: "application/octet-stream",
    size: item.encryptedSize,
    storedBytes: item.kind === "file" ? item.encryptedSize : 0,
    modified: 0,
    fileId: item.id,
    version: 1,
    trashed: false,
    trashedAt: 0,
    pcBackup: false,
    linkIds: [],
    sharedWith: item.sharedWith ?? [],
    locked: false,
    unreadable: true,
    meta: null,
  };
  let meta: Record<string, unknown>;
  try {
    meta = JSON.parse(openDriveMeta(fromBase64(item.metaCiphertext), fromBase64(item.metaNonce), key)) as Record<string, unknown>;
    if (!meta || typeof meta !== "object") return base;
  } catch {
    return base;
  }
  const links = meta.links && typeof meta.links === "object" ? Object.keys(meta.links as object) : [];
  return {
    ...base,
    name: str(meta.name, "Untitled") || "Untitled",
    parent: str(meta.parent),
    mime: str(meta.mime, "application/octet-stream"),
    size: num(meta.size, item.encryptedSize),
    modified: num(meta.modified),
    fileId: str(meta.fileId, item.id) || item.id,
    version: Math.max(1, num(meta.version, 1)),
    trashed: meta.trashed === true,
    trashedAt: num(meta.trashedAt),
    pcBackup: meta.pcBackup === true,
    linkIds: links,
    locked: meta.lock !== undefined && meta.lock !== null && meta.key === undefined,
    unreadable: false,
    meta,
  };
}

/**
 * Moves an item (every version of a file, or a folder) to the Trash the way
 * the desktop does: the same metadata with `trashed` and `trashedAt` set,
 * re-sealed on this device.
 */
export function trashedMeta(entry: DriveEntry, key: Uint8Array, now: number): { ciphertext: Uint8Array; nonce: Uint8Array } {
  if (!entry.meta) throw new Error("This item can't be changed on this device.");
  return sealDriveMeta(JSON.stringify({ ...entry.meta, trashed: true, trashedAt: now }), key);
}

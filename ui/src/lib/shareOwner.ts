// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The owner's side of public share links (docs/public-share-links.md):
// making them, keeping a folder link's items in step with Files, passwords,
// regenerating, and the files people upload through Contributor links.
// Everything sealed or opened here uses the owner's Files key (driveKey);
// the server only ever gets ciphertext and SHA-256 verifiers.
import * as api from "./api.js";
import { API_BASE } from "./apiBase.js";
import type { DriveEntry } from "./driveMeta.js";
import { sealDriveMeta } from "./driveMeta.js";
import { readStream } from "./driveFiles.js";
import { toBase64 } from "./e2eCrypto.js";
import { decryptFile } from "./fileCrypto.js";
import {
  type ContributionMeta,
  deriveShareKeys,
  fromB64,
  fromB64Url,
  newLinkSecret,
  newPasswordParams,
  newSalt,
  newShareKey,
  openContribution,
  openItem,
  openOwnerBox,
  type OwnerBox,
  sealEnvelope,
  sealItem,
  sealOwnerBox,
  type ShareItemMeta,
  shareUrl,
  toB64,
  toB64Url,
  uploadKeyPair,
  verifierOf,
} from "./shareCrypto.js";

/** Where the app is served (ends with "/"): the share page is at …/share/. */
export function webAppBase(): string {
  return `${window.location.origin}${import.meta.env.BASE_URL}`.replace(/\/?$/, "/");
}

export type ShareStatus = "active" | "expired" | "revoked";

/** A link as its owner sees it, with what their Files key opens. */
export interface OwnerShare {
  dto: api.ShareLinkDto;
  /** Null when this device can't open the owner's copy (a different account key). */
  box: OwnerBox | null;
  url: string | null;
  status: ShareStatus;
}

export function decodeOwnerShare(dto: api.ShareLinkDto, driveKey: Uint8Array): OwnerShare {
  const box = openOwnerBox(driveKey, fromB64(dto.ownerBox), fromB64(dto.ownerBoxNonce));
  let url: string | null = null;
  if (box) {
    try {
      url = shareUrl(webAppBase(), dto.publicId, fromB64Url(box.secret));
    } catch {
      url = null;
    }
  }
  return { dto, box, url, status: dto.revoked ? "revoked" : dto.expired ? "expired" : "active" };
}

export interface ShareOptions {
  permission: "viewer" | "contributor";
  label: string;
  password: string | null;
  expiresAt: Date | null;
  maxDownloads: number | null;
  upload: { maxFileSize: number | null; allowance: number | null; maxFiles: number | null } | null;
}

/** A folder link holds at most this many items (the server enforces it too). */
export const MAX_SHARE_ITEMS = 5000;
const BATCH = 200;

export class ShareTooBigError extends Error {
  constructor() {
    super(`A link can show up to ${MAX_SHARE_ITEMS.toLocaleString()} items. Share a smaller folder.`);
    this.name = "ShareTooBigError";
  }
}

const passwordDto = (p: { salt: Uint8Array; ops: number; mem: number } | null): api.SharePasswordDto | null =>
  p ? { salt: toB64(p.salt), ops: p.ops, mem: p.mem } : null;

/** Makes a link to a file or folder in Files, then adds its items. Returns the link and its URL. */
export async function createShare(root: DriveEntry, entries: DriveEntry[], options: ShareOptions, driveKey: Uint8Array): Promise<OwnerShare> {
  if (root.locked) throw new Error("This file is locked with its own key or password. Unlock it first, then share it.");
  if (options.permission === "contributor" && root.kind !== "folder") throw new Error("Only folders can take uploads.");
  const secret = newLinkSecret();
  const shareKey = newShareKey();
  const uploadSalt = newSalt();
  const params = options.password ? newPasswordParams() : null;
  const keys = await deriveShareKeys(secret, options.password, params);
  const rootItemId = crypto.randomUUID();
  // Checked before anything is created: a folder too big to share never becomes a link.
  const planned = desiredItems(root.id, rootItemId, entries, []);
  if (planned.length > MAX_SHARE_ITEMS) throw new ShareTooBigError();
  const envelope = sealEnvelope(keys.wrap, { key: shareKey, upload: uploadKeyPair(driveKey, uploadSalt).publicKey, root: rootItemId });
  const box: OwnerBox = { v: 1, secret: toB64Url(secret), key: toB64(shareKey), root: rootItemId, label: options.label.trim(), created: Date.now() };
  const owner = sealOwnerBox(driveKey, box);
  const dto = await api.createShareLink({
    backupId: root.id,
    permission: options.permission,
    lookupVerifier: verifierOf(keys.lookup),
    accessVerifier: verifierOf(keys.access),
    password: passwordDto(params),
    envelope: toB64(envelope.ciphertext),
    envelopeNonce: toB64(envelope.nonce),
    ownerBox: toB64(owner.ciphertext),
    ownerBoxNonce: toB64(owner.nonce),
    uploadKeySalt: toB64(uploadSalt),
    expiresAt: options.expiresAt?.toISOString() ?? null,
    maxDownloads: options.maxDownloads,
    upload: options.permission === "contributor" ? options.upload : null,
  });
  const share = decodeOwnerShare(dto, driveKey);
  await reconcileShare(share, entries, [], driveKey);
  return share;
}

interface DesiredItem {
  itemId: string;
  backupId: string;
  meta: ShareItemMeta;
}

/**
 * What a link should show right now: the shared file, or the folder and
 * everything in it that isn't in the Trash, with the newest version of each
 * file. Locked files are left out (their key isn't in the metadata). Item
 * ids already in the link are kept (by object, and by file across versions).
 */
export function desiredItems(rootBackupId: string, rootItemId: string, entries: DriveEntry[], existing: { itemId: string; backupId: string }[]): DesiredItem[] {
  const byId = new Map(entries.map((e) => [e.id, e]));
  const root = byId.get(rootBackupId);
  if (!root || root.trashed || root.unreadable) return [];
  const idByBackup = new Map(existing.map((i) => [i.backupId, i.itemId]));
  const idByFileId = new Map<string, string>();
  for (const i of existing) {
    const e = byId.get(i.backupId);
    if (e && e.kind === "file") idByFileId.set(e.fileId, i.itemId);
  }
  const fileMeta = (e: DriveEntry, parent: string): ShareItemMeta | null => {
    const m = e.meta ?? {};
    if (e.locked || typeof m.key !== "string" || typeof m.header !== "string" || typeof m.sha !== "string") return null;
    return {
      v: 1,
      kind: "file",
      name: e.name,
      parent,
      mime: e.mime,
      size: e.size,
      modified: Math.max(0, Math.floor(e.modified)),
      key: m.key,
      header: m.header,
      chunk: typeof m.chunk === "number" ? m.chunk : 65536,
      sha: m.sha,
    };
  };
  if (root.kind === "file") {
    const meta = fileMeta(root, "");
    return meta ? [{ itemId: rootItemId, backupId: root.id, meta }] : [];
  }

  const out: DesiredItem[] = [{ itemId: rootItemId, backupId: root.id, meta: { v: 1, kind: "folder", name: root.name, parent: "" } }];
  const children = new Map<string, DriveEntry[]>();
  for (const e of entries) {
    if (e.trashed || e.unreadable) continue;
    children.set(e.parent, [...(children.get(e.parent) ?? []), e]);
  }
  const visited = new Set<string>([root.id]);
  const queue: { folder: DriveEntry; itemId: string; depth: number }[] = [{ folder: root, itemId: rootItemId, depth: 0 }];
  while (queue.length > 0) {
    const { folder, itemId, depth } = queue.shift()!;
    const inside = children.get(folder.id) ?? [];
    const newest = new Map<string, DriveEntry>();
    for (const e of inside) {
      if (e.kind === "folder") {
        if (visited.has(e.id) || depth >= 64) continue;
        visited.add(e.id);
        const childId = idByBackup.get(e.id) ?? crypto.randomUUID();
        out.push({ itemId: childId, backupId: e.id, meta: { v: 1, kind: "folder", name: e.name, parent: itemId } });
        queue.push({ folder: e, itemId: childId, depth: depth + 1 });
      } else {
        const seen = newest.get(e.fileId);
        if (!seen || e.version > seen.version || (e.version === seen.version && e.modified > seen.modified)) newest.set(e.fileId, e);
      }
    }
    for (const e of newest.values()) {
      const meta = fileMeta(e, itemId);
      if (!meta) continue;
      out.push({ itemId: idByFileId.get(e.fileId) ?? idByBackup.get(e.id) ?? crypto.randomUUID(), backupId: e.id, meta });
    }
    if (out.length > MAX_SHARE_ITEMS + 1) break;
  }
  return out;
}

const sameMeta = (a: ShareItemMeta, b: ShareItemMeta) => JSON.stringify(a) === JSON.stringify(b);

/**
 * Brings a link's items in line with Files: adds new files and folders,
 * re-seals renamed or moved ones and new versions, and removes what was
 * trashed, deleted or moved out. `known` saves a request when the caller just
 * fetched the items.
 */
export async function reconcileShare(
  share: OwnerShare,
  entries: DriveEntry[],
  known: api.ShareOwnerItemDto[] | null,
  _driveKey: Uint8Array
): Promise<{ added: number; updated: number; removed: number }> {
  if (!share.box || share.status !== "active") return { added: 0, updated: 0, removed: 0 };
  const shareKey = fromB64(share.box.key);
  const fetched = known ?? (await api.listShareItems(share.dto.id)).items;
  const desired = desiredItems(share.dto.backupId, share.box.root, entries, fetched);
  if (desired.length > MAX_SHARE_ITEMS) throw new ShareTooBigError();
  const current = new Map(fetched.map((i) => [i.itemId, i]));
  const wanted = new Set(desired.map((d) => d.itemId));
  const upserts: { itemId: string; backupId: string; metaCiphertext: string; metaNonce: string }[] = [];
  let added = 0;
  let updated = 0;
  for (const d of desired) {
    const have = current.get(d.itemId);
    if (have && have.backupId === d.backupId) {
      const meta = openItem(shareKey, have.itemId, fromB64(have.metaCiphertext), fromB64(have.metaNonce));
      if (meta && sameMeta(meta, d.meta)) continue;
      updated += 1;
    } else if (have) {
      updated += 1;
    } else {
      added += 1;
    }
    const sealed = sealItem(shareKey, d.itemId, d.meta);
    upserts.push({ itemId: d.itemId, backupId: d.backupId, metaCiphertext: toB64(sealed.ciphertext), metaNonce: toB64(sealed.nonce) });
  }
  const removals = fetched.filter((i) => !wanted.has(i.itemId)).map((i) => i.itemId);
  for (let i = 0; i < removals.length; i += BATCH) await api.removeShareItems(share.dto.id, removals.slice(i, i + BATCH));
  for (let i = 0; i < upserts.length; i += BATCH) await api.upsertShareItems(share.dto.id, share.dto.keyEpoch, upserts.slice(i, i + BATCH));
  return { added, updated, removed: removals.length };
}

/** Keeps every live link in step with Files (after uploads, renames, moves, trash). Errors on one link don't stop the rest. */
export async function reconcileAll(shares: OwnerShare[], entries: DriveEntry[], driveKey: Uint8Array): Promise<void> {
  for (const share of shares) {
    if (share.status !== "active" || !share.box) continue;
    try {
      await reconcileShare(share, entries, null, driveKey);
    } catch {
      // Tried again on the next refresh.
    }
  }
}

/** Sets, changes or removes the password: a new access token and envelope; the share key and items stay. */
export async function setSharePassword(share: OwnerShare, password: string | null, driveKey: Uint8Array): Promise<OwnerShare> {
  if (!share.box) throw new Error("This link can't be changed on this device.");
  const params = password ? newPasswordParams() : null;
  const keys = await deriveShareKeys(fromB64Url(share.box.secret), password, params);
  const envelope = sealEnvelope(keys.wrap, {
    key: fromB64(share.box.key),
    upload: uploadKeyPair(driveKey, fromB64(share.dto.uploadKeySalt)).publicKey,
    root: share.box.root,
  });
  const dto = await api.setShareLinkPassword(share.dto.id, {
    password: passwordDto(params),
    accessVerifier: verifierOf(keys.access),
    envelope: toB64(envelope.ciphertext),
    envelopeNonce: toB64(envelope.nonce),
  });
  return decodeOwnerShare(dto, driveKey);
}

/**
 * A new link for the same item: a new address, secret and share key, every
 * item sealed again. The old link, and downloads started with it, stop working.
 */
export async function regenerateShare(share: OwnerShare, password: string | null, entries: DriveEntry[], driveKey: Uint8Array): Promise<OwnerShare> {
  if (!share.box) throw new Error("This link can't be changed on this device.");
  const secret = newLinkSecret();
  const shareKey = newShareKey();
  const params = password ? newPasswordParams() : null;
  const keys = await deriveShareKeys(secret, password, params);
  const rootItemId = crypto.randomUUID();
  const envelope = sealEnvelope(keys.wrap, {
    key: shareKey,
    upload: uploadKeyPair(driveKey, fromB64(share.dto.uploadKeySalt)).publicKey,
    root: rootItemId,
  });
  const owner = sealOwnerBox(driveKey, { ...share.box, secret: toB64Url(secret), key: toB64(shareKey), root: rootItemId });
  const dto = await api.regenerateShareLink(share.dto.id, {
    lookupVerifier: verifierOf(keys.lookup),
    accessVerifier: verifierOf(keys.access),
    password: passwordDto(params),
    envelope: toB64(envelope.ciphertext),
    envelopeNonce: toB64(envelope.nonce),
    ownerBox: toB64(owner.ciphertext),
    ownerBoxNonce: toB64(owner.nonce),
  });
  const next = decodeOwnerShare(dto, driveKey);
  await reconcileShare(next, entries, [], driveKey);
  return next;
}

/** Renames the link for its owner (the label lives only in the owner's sealed copy). */
export async function relabelShare(share: OwnerShare, label: string, driveKey: Uint8Array): Promise<OwnerShare> {
  if (!share.box) throw new Error("This link can't be changed on this device.");
  const owner = sealOwnerBox(driveKey, { ...share.box, label: label.trim() });
  const dto = await api.updateShareLink(share.dto.id, { ownerBox: toB64(owner.ciphertext), ownerBoxNonce: toB64(owner.nonce) });
  return decodeOwnerShare(dto, driveKey);
}

// --- Incoming ---

export interface IncomingFile {
  dto: api.IncomingFileDto;
  /** Null when it can't be opened on this device. */
  meta: ContributionMeta | null;
  /** The file's own name (the uploader's folder path, if any, is in `folder`). */
  name: string;
  folder: string;
}

export function decodeIncoming(dto: api.IncomingFileDto, driveKey: Uint8Array): IncomingFile {
  let meta: ContributionMeta | null = null;
  try {
    meta = openContribution(uploadKeyPair(driveKey, fromB64(dto.keySalt)), fromB64(dto.sealedMeta));
  } catch {
    meta = null;
  }
  // The server's hash and the sealed one must agree: nothing else can be swapped in.
  if (meta && meta.sha !== dto.ciphertextSha256) meta = null;
  const parts = (meta?.name ?? "").split("/").filter(Boolean);
  return { dto, meta, name: parts.pop() ?? "Encrypted file", folder: parts.join("/") };
}

/** Downloads an incoming file and decrypts it here. Untrusted: never opened automatically. */
export async function downloadIncoming(file: IncomingFile, onProgress?: (fraction: number) => void): Promise<Blob> {
  if (!file.meta) throw new Error("This file can't be opened on this device.");
  const res = await fetch(`${API_BASE}/shares/incoming/${encodeURIComponent(file.dto.id)}/content`, { credentials: "include", cache: "no-store" });
  if (!res.ok || !res.body) throw new Error("Couldn't download the file. Try again.");
  return decryptFile(
    readStream(res.body),
    {
      key: fromB64(file.meta.key),
      header: fromB64(file.meta.header),
      chunkSize: file.meta.chunk,
      encryptedSize: file.dto.encryptedSize,
      ciphertextSha256: file.meta.sha,
    },
    "application/octet-stream",
    onProgress
  );
}

/** Moves an incoming file into Files (into `parent`), re-sealing its details with the Files key. No bytes move. */
export async function acceptIncoming(file: IncomingFile, parent: string, driveKey: Uint8Array): Promise<api.BackupItemDto> {
  if (!file.meta) throw new Error("This file can't be opened on this device.");
  const meta = {
    v: 1,
    kind: "file",
    name: file.name,
    parent,
    modified: Date.now(),
    trashed: false,
    trashedAt: 0,
    mime: file.meta.mime,
    size: file.meta.size,
    fileId: crypto.randomUUID(),
    version: 1,
    key: file.meta.key,
    header: file.meta.header,
    chunk: file.meta.chunk,
    sha: file.meta.sha,
  };
  const sealed = sealDriveMeta(JSON.stringify(meta), driveKey);
  return (await api.acceptIncomingFile(file.dto.id, toBase64(sealed.ciphertext), toBase64(sealed.nonce))).item;
}

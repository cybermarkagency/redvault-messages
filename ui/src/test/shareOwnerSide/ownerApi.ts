// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Test support only: the owner's side of Files and public share links, which
// lives in the apps now (the website no longer has Files). The share page's
// end-to-end test (shareEndToEnd.test.ts) uses it to make real shares
// against the real server, so the share page is still tested end to end.
import { ApiError } from "../../lib/api.js";
import { API_BASE } from "../../lib/apiBase.js";
import type { SubscriptionTier } from "../../types.js";

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, {
    ...init,
    credentials: "include",
    headers: { "Content-Type": "application/json", ...init?.headers },
  });
  if (res.status === 204) return undefined as T;
  const body = await res.json().catch(() => null);
  if (!res.ok) throw new ApiError(res.status, body?.error ?? "unknown_error", body?.message ?? res.statusText);
  return body as T;
}

// --- Files (encrypted cloud storage) and storage analytics ---
// Everything here is ciphertext plus sizes: names, folders, types and dates
// are inside metaCiphertext, which only this device can open (lib/driveMeta.ts).

export interface BackupItemDto {
  id: string;
  kind: "file" | "folder";
  encryptedSize: number;
  ciphertextSha256: string | null;
  metaCiphertext: string;
  metaNonce: string;
  /** Who the file is shared with directly (user ids). */
  sharedWith: string[];
}

export interface BackupUsageDto {
  used: number;
  limit: number;
  remaining: number;
  tier: SubscriptionTier;
}

export interface BackupLinkDto {
  id: string;
  backupId: string;
  hasPassword: boolean;
  allowDownload: boolean;
  expiresAt: string | null;
}

export function listBackupItems(): Promise<{ items: BackupItemDto[]; usage: BackupUsageDto }> {
  return request("/backup/items");
}

export function listBackupLinks(): Promise<{ links: BackupLinkDto[] }> {
  return request("/backup/links");
}

export function deleteBackupItem(id: string): Promise<void> {
  return request(`/backup/${encodeURIComponent(id)}`, { method: "DELETE" });
}

/** Rename, move, trash or restore: the metadata, re-sealed on this device. */
export function updateBackupMeta(id: string, metaCiphertext: string, metaNonce: string): Promise<BackupItemDto> {
  return request(`/backup/${encodeURIComponent(id)}/meta`, { method: "PUT", body: JSON.stringify({ metaCiphertext, metaNonce }) });
}

export function deleteBackupLink(linkId: string): Promise<void> {
  return request(`/backup/links/${encodeURIComponent(linkId)}`, { method: "DELETE" });
}

export interface BackupTransferDto {
  mode: "proxy" | "direct";
  url: string;
  method: "PUT" | "GET";
  headers: Record<string, string>;
  expiresAt: string;
}

/** Room for one encrypted file in Files: its exact encrypted size, hash and sealed metadata. */
export function requestBackupUpload(input: {
  encryptedSize: number;
  ciphertextSha256: string;
  metaCiphertext: string;
  metaNonce: string;
}): Promise<{ objectId: string; upload: BackupTransferDto }> {
  return request("/backup/upload-request", { method: "POST", body: JSON.stringify(input) });
}

export function createBackupFolder(metaCiphertext: string, metaNonce: string): Promise<BackupItemDto> {
  return request("/backup/folders", { method: "POST", body: JSON.stringify({ metaCiphertext, metaNonce }) });
}

export function completeBackupUpload(id: string): Promise<BackupItemDto> {
  return request(`/backup/${encodeURIComponent(id)}/complete`, { method: "POST", body: "{}" });
}

export function backupDownloadTarget(id: string): Promise<{ download: BackupTransferDto }> {
  return request(`/backup/${encodeURIComponent(id)}/download`);
}

// --- Public share links (docs/public-share-links.md) ---

export interface SharePasswordDto {
  salt: string;
  ops: number;
  mem: number;
}

export interface ShareLinkDto {
  id: string;
  publicId: string;
  /** The shared file or folder (the owner's own storage id; never in the link). */
  backupId: string;
  kind: "file" | "folder";
  permission: "viewer" | "contributor";
  keyEpoch: number;
  password: SharePasswordDto | null;
  envelope: string;
  envelopeNonce: string;
  ownerBox: string;
  ownerBoxNonce: string;
  uploadKeySalt: string;
  expiresAt: string | null;
  expired: boolean;
  revoked: boolean;
  maxDownloads: number | null;
  downloadCount: number;
  openCount: number;
  upload: { maxFileSize: number | null; allowance: number | null; maxFiles: number | null; count: number; bytes: number; inProgress: number };
  /** Files that came in through this link and wait in Incoming. */
  incoming: number;
}

export interface NewShareLinkInput {
  backupId: string;
  permission: "viewer" | "contributor";
  lookupVerifier: string;
  accessVerifier: string;
  password: SharePasswordDto | null;
  envelope: string;
  envelopeNonce: string;
  ownerBox: string;
  ownerBoxNonce: string;
  uploadKeySalt: string;
  expiresAt: string | null;
  maxDownloads: number | null;
  upload: { maxFileSize: number | null; allowance: number | null; maxFiles: number | null } | null;
}

export interface ShareSettingsInput {
  permission?: "viewer" | "contributor";
  expiresAt?: string | null;
  maxDownloads?: number | null;
  upload?: { maxFileSize?: number | null; allowance?: number | null; maxFiles?: number | null };
  ownerBox?: string;
  ownerBoxNonce?: string;
}

export interface ShareOwnerItemDto {
  itemId: string;
  backupId: string;
  kind: "file" | "folder";
  metaCiphertext: string;
  metaNonce: string;
}

export interface IncomingFileDto {
  id: string;
  /** Null once the link it came through was deleted. */
  shareId: string | null;
  encryptedSize: number;
  ciphertextSha256: string;
  sealedMeta: string;
  keySalt: string;
}

export function listShareLinks(): Promise<{ links: ShareLinkDto[]; incoming: number }> {
  return request("/shares");
}

export function createShareLink(input: NewShareLinkInput): Promise<ShareLinkDto> {
  return request("/shares", { method: "POST", body: JSON.stringify(input) });
}

export function updateShareLink(id: string, input: ShareSettingsInput): Promise<ShareLinkDto> {
  return request(`/shares/${encodeURIComponent(id)}`, { method: "PATCH", body: JSON.stringify(input) });
}

export function setShareLinkPassword(
  id: string,
  input: { password: SharePasswordDto | null; accessVerifier: string; envelope: string; envelopeNonce: string }
): Promise<ShareLinkDto> {
  return request(`/shares/${encodeURIComponent(id)}/password`, { method: "PUT", body: JSON.stringify(input) });
}

export function regenerateShareLink(
  id: string,
  input: {
    lookupVerifier: string;
    accessVerifier: string;
    password: SharePasswordDto | null;
    envelope: string;
    envelopeNonce: string;
    ownerBox: string;
    ownerBoxNonce: string;
  }
): Promise<ShareLinkDto> {
  return request(`/shares/${encodeURIComponent(id)}/regenerate`, { method: "POST", body: JSON.stringify(input) });
}

export function revokeShareLink(id: string): Promise<ShareLinkDto> {
  return request(`/shares/${encodeURIComponent(id)}/revoke`, { method: "POST", body: "{}" });
}

export function deleteShareLink(id: string): Promise<void> {
  return request(`/shares/${encodeURIComponent(id)}`, { method: "DELETE" });
}

export function listShareItems(id: string): Promise<{ keyEpoch: number; items: ShareOwnerItemDto[] }> {
  return request(`/shares/${encodeURIComponent(id)}/items`);
}

export function upsertShareItems(
  id: string,
  keyEpoch: number,
  items: { itemId: string; backupId: string; metaCiphertext: string; metaNonce: string }[]
): Promise<{ stored: number }> {
  return request(`/shares/${encodeURIComponent(id)}/items`, { method: "POST", body: JSON.stringify({ keyEpoch, items }) });
}

export function removeShareItems(id: string, itemIds: string[]): Promise<{ removed: number }> {
  return request(`/shares/${encodeURIComponent(id)}/items/remove`, { method: "POST", body: JSON.stringify({ itemIds }) });
}

export function listIncomingFiles(): Promise<{ items: IncomingFileDto[] }> {
  return request("/shares/incoming");
}

export function acceptIncomingFile(id: string, metaCiphertext: string, metaNonce: string): Promise<{ item: BackupItemDto }> {
  return request(`/shares/incoming/${encodeURIComponent(id)}/accept`, { method: "POST", body: JSON.stringify({ metaCiphertext, metaNonce }) });
}

export function rejectIncomingFile(id: string): Promise<void> {
  return request(`/shares/incoming/${encodeURIComponent(id)}/reject`, { method: "POST", body: "{}" });
}


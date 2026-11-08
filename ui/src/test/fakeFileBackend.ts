// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// In-memory stand-in for the encrypted-file endpoints (api.ts) and the
// blob transfer layer (fileTransfer.ts), layered over ./fakeApiBackend.js.
// Records every argument that crosses the "network" boundary so tests can
// prove what the server/storage would — and would not — ever receive.
import { vi } from "vitest";
import type * as ApiModule from "../lib/api.js";
import type { TransferTargetDto } from "../lib/fileTransfer.js";

export interface RecordedCall {
  fn: string;
  args: unknown[];
}

/** Every API call's arguments, in order. */
export const apiCalls: RecordedCall[] = [];
/** Ciphertext as "stored in B2", by object id. */
export const storedObjects = new Map<string, Uint8Array>();
/** What saveBlobAs handed the user. */
export const savedFiles: { filename: string; bytes: Uint8Array; type: string }[] = [];

interface FakeFile {
  uploaderUserId: string;
  conversationId: string;
  encryptedSize: number;
  status: "pending" | "uploaded" | "available" | "deleted" | "expired" | "failed";
}
const files = new Map<string, FakeFile>();
const attachmentsByMessage = new Map<string, string[]>();
let seq = 0;

export const FAKE_MAX_FILE_SIZE = 5 * 1024 * 1024;
/** The Pro storage limit. Free accounts can't upload at all (limit 0), mirroring FileService.storageLimitFor. */
export const FAKE_STORAGE_LIMIT = 20 * 1024 * 1024;
const LIVE_STATUSES = new Set<FakeFile["status"]>(["pending", "uploaded", "available"]);

export function resetFakeFileBackend(): void {
  apiCalls.length = 0;
  storedObjects.clear();
  savedFiles.length = 0;
  files.clear();
  attachmentsByMessage.clear();
}

export function setFakeFileStatus(objectId: string, status: FakeFile["status"]): void {
  const f = files.get(objectId);
  if (f) f.status = status;
}

export function readBlob(blob: Blob): Promise<Uint8Array> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error);
    reader.readAsArrayBuffer(blob);
  });
}

function attachmentDtos(messageId: string) {
  return (attachmentsByMessage.get(messageId) ?? []).map((objectId) => ({
    objectId,
    status: files.get(objectId)?.status ?? "deleted",
    encryptedSize: files.get(objectId)?.encryptedSize ?? null,
  }));
}

type Api = typeof ApiModule;

/** fakeApiModule + file endpoints, with every function wrapped to record its arguments. */
export function withFakeFileApi(base: Record<string, unknown>): Record<string, unknown> {
  const ApiError = base.ApiError as new (status: number, code: string, message: string) => Error;
  const baseMe = base.me as Api["me"];
  const baseSend = base.sendMessage as Api["sendMessage"];
  const baseList = base.listMessages as Api["listMessages"];

  const extra = {
    getFileConfig: async () => ({
      maxFileSize: FAKE_MAX_FILE_SIZE,
      maxEncryptedSize: FAKE_MAX_FILE_SIZE + 2000,
      maxAttachmentsPerMessage: 5,
      retentionDays: 30,
      transferMode: "proxy" as const,
      proStorageLimitBytes: FAKE_STORAGE_LIMIT,
    }),
    getFileStorageUsage: async () => {
      const me = await baseMe();
      const used = [...files.values()]
        .filter((f) => f.uploaderUserId === me.userId && LIVE_STATUSES.has(f.status))
        .reduce((sum, f) => sum + f.encryptedSize, 0);
      return { used, limit: me.subscriptionTier === "pro" ? FAKE_STORAGE_LIMIT : 0 };
    },
    requestFileUpload: async (input: { conversationId: string; encryptedSize: number; ciphertextSha256: string }) => {
      const me = await baseMe();
      if (me.subscriptionTier !== "pro") {
        throw new ApiError(403, "pro_required", "Sending files is a Pro feature — upgrade to Pro to send files.");
      }
      // Mirrors FileService.requestUpload's storage cap — same rejection
      // code/message the real server uses, so UI tests can prove the
      // upgrade/cleanup message actually reaches the user.
      const used = [...files.values()]
        .filter((f) => f.uploaderUserId === me.userId && LIVE_STATUSES.has(f.status))
        .reduce((sum, f) => sum + f.encryptedSize, 0);
      if (used + input.encryptedSize > FAKE_STORAGE_LIMIT) {
        throw new ApiError(429, "storage_limit_exceeded", "Storage limit reached — delete some files to free up space, or upgrade to Pro for more room.");
      }
      const objectId = `00000000-0000-4000-8000-${String(++seq).padStart(12, "0")}`;
      files.set(objectId, {
        uploaderUserId: me.userId,
        conversationId: input.conversationId,
        encryptedSize: input.encryptedSize,
        status: "pending",
      });
      const upload: TransferTargetDto = {
        mode: "proxy",
        url: `/v1/files/${objectId}/content`,
        method: "PUT",
        headers: { "Content-Type": "application/octet-stream" },
        expiresAt: new Date(Date.now() + 900_000).toISOString(),
      };
      return { objectId, upload, maxEncryptedSize: FAKE_MAX_FILE_SIZE + 2000 };
    },
    completeFileUpload: async (objectId: string) => {
      const f = files.get(objectId);
      if (!f || !storedObjects.has(objectId)) throw new ApiError(409, "upload_incomplete", "Upload not found");
      if (f.status === "pending") f.status = "uploaded";
      return { objectId, status: f.status, encryptedSize: f.encryptedSize };
    },
    getFileDownload: async (objectId: string) => {
      const f = files.get(objectId);
      if (!f) throw new ApiError(404, "not_found", "File not found");
      if (f.status === "deleted") throw new ApiError(410, "file_deleted", "This file was deleted.");
      if (f.status === "expired") throw new ApiError(410, "file_expired", "This file has expired.");
      const download: TransferTargetDto = {
        mode: "proxy",
        url: `/v1/files/${objectId}/content`,
        method: "GET",
        headers: {},
        expiresAt: new Date(Date.now() + 300_000).toISOString(),
      };
      return { objectId, encryptedSize: f.encryptedSize, download };
    },
    deleteFile: async (objectId: string) => {
      setFakeFileStatus(objectId, "deleted");
      storedObjects.delete(objectId);
    },
    sendMessage: async (conversationId: string, ciphertext: string, nonce: string, attachments?: string[]) => {
      const dto = await baseSend(conversationId, ciphertext, nonce);
      if (attachments?.length) {
        attachmentsByMessage.set(dto.id, attachments);
        for (const id of attachments) setFakeFileStatus(id, "available");
      }
      return { ...dto, attachments: attachmentDtos(dto.id) };
    },
    listMessages: async (conversationId: string, options?: { limit?: number }) => {
      const dtos = await baseList(conversationId, options);
      return dtos.map((d) => ({ ...d, attachments: attachmentDtos(d.id) }));
    },
  };

  const merged: Record<string, unknown> = { ...base, ...extra };
  for (const [name, value] of Object.entries(merged)) {
    if (typeof value !== "function" || name === "ApiError") continue;
    merged[name] = (...args: unknown[]) => {
      apiCalls.push({ fn: name, args });
      return (value as (...a: unknown[]) => unknown)(...args);
    };
  }
  return merged;
}

/** Replacement for ../lib/fileTransfer.js: "B2" is storedObjects; the real TransferError class is kept. */
export async function fakeFileTransferModule() {
  const actual = await vi.importActual<typeof import("../lib/fileTransfer.js")>("../lib/fileTransfer.js");
  return {
    ...actual,
    uploadCiphertext: async (target: TransferTargetDto, blob: Blob) => {
      const objectId = target.url.split("/")[3];
      storedObjects.set(objectId, await readBlob(blob));
    },
    downloadCiphertext: async (target: TransferTargetDto) => {
      const objectId = target.url.split("/")[3];
      const bytes = storedObjects.get(objectId);
      if (!bytes) throw new actual.TransferError("gone", "This file is no longer available.", 410, "file_deleted");
      return (async function* () {
        // Deliver in uneven pieces, like a real network stream.
        for (let off = 0; off < bytes.length; off += 10_007) yield bytes.slice(off, off + 10_007);
      })();
    },
    saveBlobAs: (blob: Blob, filename: string) => {
      void readBlob(blob).then((bytes) => savedFiles.push({ filename, bytes, type: blob.type }));
    },
  };
}

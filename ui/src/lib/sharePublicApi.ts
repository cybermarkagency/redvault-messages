// The share page's calls to /api/public/shares (docs/public-share-links.md).
// No cookie, no referrer, nothing cached: opening a link never involves an
// account, and the link's secret never leaves this page (only tokens derived
// from it do, in headers).
import { API_BASE } from "./apiBase.js";
import { toB64Url } from "./shareCrypto.js";

export interface ShareLookupDto {
  kind: "file" | "folder";
  password: { salt: string; ops: number; mem: number } | null;
}

export interface ShareItemDto {
  id: string;
  kind: "file" | "folder";
  encryptedSize: number;
  metaCiphertext: string;
  metaNonce: string;
}

export interface ShareUploadLimitsDto {
  maxFileSize: number | null;
  maxEncryptedFileSize: number;
  remainingBytes: number | null;
  remainingFiles: number | null;
  minPartSize: number;
  maxPartSize: number;
}

export interface ShareContentsDto {
  kind: "file" | "folder";
  permission: "viewer" | "contributor";
  envelope: string;
  envelopeNonce: string;
  expiresAt: string | null;
  items: ShareItemDto[];
  downloads: { limit: number | null; remaining: number | null };
  uploads: ShareUploadLimitsDto | null;
}

export interface ShareTicketDto {
  ticket: string;
  expiresAt: string;
  downloads: { limit: number | null; remaining: number | null };
}

export interface ShareUploadDto {
  uploadId: string;
  uploadToken: string;
  partSize: number;
  partCount: number;
  expiresAt: string;
}

/** Something the recipient can read as-is, with a code the page can act on. */
export class ShareLinkError extends Error {
  constructor(
    message: string,
    readonly code: string,
    readonly status = 0,
    readonly retryAfter: number | null = null
  ) {
    super(message);
    this.name = "ShareLinkError";
  }

  /** Worth trying again by itself (the network, a busy server). */
  get transient(): boolean {
    return this.code === "network" || this.status === 429 || this.status >= 500;
  }
}

const MESSAGES: Record<string, string> = {
  not_found: "This link doesn't exist, or its owner removed it.",
  share_revoked: "This link was turned off by its owner.",
  share_expired: "This link has expired. Ask its owner for a new one.",
  share_unavailable: "What this link shared is no longer available.",
  share_password_wrong: "Wrong password.",
  unauthorized: "This link is incomplete. Copy the whole link and try again.",
  share_download_limit: "This link has reached its download limit.",
  share_read_only: "This link doesn't take uploads.",
  share_file_too_large: "This file is larger than this link allows.",
  share_allowance_exceeded: "This file doesn't fit in what's left of this link's upload allowance.",
  share_owner_storage_full: "There isn't enough room in the owner's storage for this file.",
  share_upload_limit: "This link has taken as many files as its owner allows.",
  share_busy: "Too many uploads are in progress on this link. Try again in a few minutes.",
  share_ticket_invalid: "This download expired. Start it again.",
  share_epoch_changed: "This link was replaced by a new one. Ask its owner for it.",
  share_upload_corrupt: "The file didn't arrive intact. Upload it again.",
  share_upload_ended: "This upload was stopped. Start it again.",
  backup_disabled: "Files aren't available on this server right now.",
  rate_limited: "Too many requests. Wait a minute and try again.",
};

async function failure(res: Response): Promise<ShareLinkError> {
  let code = "";
  let message = "";
  let retryAfter: number | null = null;
  try {
    const body = (await res.json()) as { error?: string; message?: string; details?: { retryAfter?: number } };
    code = body.error ?? "";
    message = body.message ?? "";
    retryAfter = typeof body.details?.retryAfter === "number" ? body.details.retryAfter : null;
  } catch {
    // Not JSON: fall back to the status.
  }
  const header = Number(res.headers.get("Retry-After"));
  if (retryAfter === null && Number.isFinite(header) && header > 0) retryAfter = header;
  if (res.status === 404 && !code.startsWith("share_")) code = "not_found";
  if (code === "share_password_locked") {
    const wait = retryAfter ?? 60;
    return new ShareLinkError(
      `Too many wrong passwords. Try again in ${wait >= 90 ? `${Math.ceil(wait / 60)} minutes` : `${wait} seconds`}.`,
      code,
      res.status,
      wait
    );
  }
  const text = MESSAGES[code] ?? (res.status >= 500 ? "The server had a problem. Try again." : message || "Something went wrong. Try again.");
  return new ShareLinkError(text, code || `http_${res.status}`, res.status, retryAfter);
}

function linkPath(publicId: string, suffix = ""): string {
  return `${API_BASE}/public/shares/${encodeURIComponent(publicId)}${suffix}`;
}

export async function shareFetch(publicId: string, suffix: string, init: RequestInit = {}): Promise<Response> {
  let res: Response;
  try {
    res = await fetch(linkPath(publicId, suffix), { ...init, credentials: "omit", referrerPolicy: "no-referrer", cache: "no-store" });
  } catch (err) {
    if ((err as { name?: string }).name === "AbortError") throw err;
    throw new ShareLinkError("Couldn't reach the server. Check your connection.", "network");
  }
  if (!res.ok) throw await failure(res);
  return res;
}

async function json<T>(publicId: string, suffix: string, init: RequestInit = {}): Promise<T> {
  const res = await shareFetch(publicId, suffix, init);
  return res.status === 204 ? (undefined as T) : ((await res.json()) as T);
}

const jsonBody = (body: unknown, headers: Record<string, string>): RequestInit => ({
  method: "POST",
  headers: { "Content-Type": "application/json", ...headers },
  body: JSON.stringify(body),
});

export const accessHeaders = (access: Uint8Array): Record<string, string> => ({ "X-Share-Access": toB64Url(access) });

export function lookupShare(publicId: string, lookup: Uint8Array): Promise<ShareLookupDto> {
  return json(publicId, "", { headers: { "X-Share-Lookup": toB64Url(lookup) } });
}

export function fetchContents(publicId: string, access: Uint8Array): Promise<ShareContentsDto> {
  return json(publicId, "/contents", { headers: accessHeaders(access) });
}

export function requestTicket(publicId: string, access: Uint8Array, items: string[] | "all"): Promise<ShareTicketDto> {
  return json(publicId, "/downloads", jsonBody({ items }, accessHeaders(access)));
}

export function startUpload(publicId: string, access: Uint8Array, encryptedSize: number, partSize: number, signal?: AbortSignal): Promise<ShareUploadDto> {
  return json(publicId, "/uploads", { ...jsonBody({ encryptedSize, partSize }, accessHeaders(access)), signal });
}

const uploadHeaders = (access: Uint8Array, token: string) => ({ ...accessHeaders(access), "X-Share-Upload": token });

export async function putPart(
  publicId: string,
  access: Uint8Array,
  upload: ShareUploadDto,
  partNumber: number,
  bytes: Uint8Array,
  sha256Hex: string,
  signal?: AbortSignal
): Promise<void> {
  await shareFetch(publicId, `/uploads/${upload.uploadId}/parts/${partNumber}`, {
    method: "PUT",
    headers: { ...uploadHeaders(access, upload.uploadToken), "Content-Type": "application/octet-stream", "X-Content-Sha256": sha256Hex },
    body: bytes as BodyInit,
    signal,
  });
}

export async function uploadStatus(publicId: string, access: Uint8Array, upload: ShareUploadDto, signal?: AbortSignal): Promise<number[]> {
  const body = await json<{ received: { partNumber: number }[] }>(publicId, `/uploads/${upload.uploadId}`, {
    headers: uploadHeaders(access, upload.uploadToken),
    signal,
  });
  return body.received.map((p) => p.partNumber);
}

export function completeUpload(
  publicId: string,
  access: Uint8Array,
  upload: ShareUploadDto,
  ciphertextSha256: string,
  sealedMeta: string,
  signal?: AbortSignal
): Promise<{ status: string }> {
  return json(publicId, `/uploads/${upload.uploadId}/complete`, {
    ...jsonBody({ ciphertextSha256, sealedMeta }, uploadHeaders(access, upload.uploadToken)),
    signal,
  });
}

export async function cancelUpload(publicId: string, access: Uint8Array, upload: ShareUploadDto): Promise<void> {
  await json(publicId, `/uploads/${upload.uploadId}`, { method: "DELETE", headers: uploadHeaders(access, upload.uploadToken) });
}

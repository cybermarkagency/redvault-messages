// Moves encrypted file blobs to/from wherever the API's transfer target
// says — only ever ciphertext. Two shapes (see server
// storage/fileObjectStorage.ts):
//   - proxy  (default): `url` is a path relative to the API base; sent with
//     the session cookie, so the storage provider never sees our users' IPs.
//   - direct: `url` is an absolute, short-lived presigned B2 URL; sent with
//     NO credentials and no referrer — the signature is the authorization,
//     and our session cookie must never go to a third-party origin.
// Transient failures (network drop, 5xx, 429) are retried with backoff;
// both operations are idempotent on the server side.
import { API_BASE } from "./apiBase.js";

export interface TransferTargetDto {
  mode: "proxy" | "direct";
  url: string;
  method: "PUT" | "GET";
  headers: Record<string, string>;
  expiresAt: string;
}

export class TransferError extends Error {
  constructor(
    public readonly kind: "network" | "expired" | "rejected" | "not_found" | "gone",
    message: string,
    public readonly status?: number,
    public readonly code?: string
  ) {
    super(message);
    this.name = "TransferError";
  }
}

const MAX_ATTEMPTS = 4;

function resolve(target: TransferTargetDto): { url: string; init: RequestInit } {
  if (target.mode === "proxy") {
    if (!target.url.startsWith("/v1/files/")) throw new TransferError("rejected", "Unexpected transfer URL");
    return { url: `${API_BASE}${target.url}`, init: { credentials: "include" } };
  }
  // Direct: only ever https, and never with our cookies attached.
  if (!/^https:\/\//i.test(target.url)) throw new TransferError("rejected", "Unexpected transfer URL");
  return { url: target.url, init: { credentials: "omit", referrerPolicy: "no-referrer", mode: "cors" } };
}

function sleep(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

async function errorFor(res: Response, target: TransferTargetDto): Promise<TransferError> {
  const body = (await res.json().catch(() => null)) as { error?: string; message?: string } | null;
  const code = body?.error;
  if (res.status === 410) return new TransferError("gone", body?.message ?? "This file is no longer available.", 410, code);
  if (res.status === 404) return new TransferError("not_found", "File not found.", 404, code);
  // A presigned URL past its expiry comes back 403 from B2.
  if (res.status === 403 && target.mode === "direct") return new TransferError("expired", "The transfer link expired.", 403, code);
  return new TransferError("rejected", body?.message ?? `Transfer failed (${res.status}).`, res.status, code);
}

async function withRetry(target: TransferTargetDto, attempt: () => Promise<Response>): Promise<Response> {
  let lastError: TransferError | null = null;
  for (let i = 0; i < MAX_ATTEMPTS; i++) {
    if (i > 0) await sleep(Math.min(8000, 500 * 2 ** (i - 1)) + Math.random() * 250);
    if (new Date(target.expiresAt).getTime() < Date.now()) {
      throw new TransferError("expired", "The transfer link expired.");
    }
    let res: Response;
    try {
      res = await attempt();
    } catch {
      lastError = new TransferError("network", "Network interrupted.");
      continue;
    }
    if (res.ok) return res;
    const err = await errorFor(res, target);
    if (res.status >= 500 || res.status === 429) {
      lastError = err;
      continue;
    }
    throw err;
  }
  throw lastError ?? new TransferError("network", "Network interrupted.");
}

export async function uploadCiphertext(target: TransferTargetDto, blob: Blob): Promise<void> {
  if (target.method !== "PUT") throw new TransferError("rejected", "Unexpected transfer method");
  const { url, init } = resolve(target);
  await withRetry(target, () => fetch(url, { ...init, method: "PUT", headers: target.headers, body: blob }));
}

/** Streams the ciphertext back as chunks, so large files aren't buffered twice. */
export async function downloadCiphertext(target: TransferTargetDto): Promise<AsyncIterable<Uint8Array>> {
  if (target.method !== "GET") throw new TransferError("rejected", "Unexpected transfer method");
  const { url, init } = resolve(target);
  const res = await withRetry(target, () => fetch(url, { ...init, method: "GET", headers: target.headers, cache: "no-store" }));
  const body = res.body;
  if (!body) {
    const buf = new Uint8Array(await res.arrayBuffer());
    return (async function* () {
      yield buf;
    })();
  }
  const reader = body.getReader();
  return (async function* () {
    try {
      for (;;) {
        let chunk: ReadableStreamReadResult<Uint8Array>;
        try {
          chunk = await reader.read();
        } catch {
          throw new TransferError("network", "Network interrupted.");
        }
        if (chunk.done) return;
        yield chunk.value;
      }
    } finally {
      reader.releaseLock();
    }
  })();
}

/** Hands a decrypted file to the user as a download. Always application/octet-stream + `download`, so the browser saves it rather than rendering it (no HTML/SVG from a sender ever runs in our origin). The object URL is revoked right after. */
export function saveBlobAs(blob: Blob, filename: string): void {
  const safe = blob.type === "application/octet-stream" ? blob : new Blob([blob], { type: "application/octet-stream" });
  const url = URL.createObjectURL(safe);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.rel = "noopener";
  a.style.display = "none";
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30_000);
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The recipient's side of a share link: opening it, reading its folder
// tree, downloading (resumable, decrypted here) and, for Contributor links,
// uploading (encrypted here, part by part, resumable). See
// docs/public-share-links.md.
import sodium from "libsodium-wrappers-sumo";
import { blobBytes } from "./blobBytes.js";
import { decryptFile, encryptedSizeFor, FILE_CHUNK_BYTES, FileDecryptError } from "./fileCrypto.js";
import {
  deriveShareKeys,
  fromB64,
  lookupToken,
  openEnvelope,
  openItem,
  ready,
  sealContribution,
  ShareCryptoError,
  type ShareEnvelope,
  type ShareFileMeta,
  type ShareItemMeta,
  type ShareKeys,
  toB64,
} from "./shareCrypto.js";
import {
  accessHeaders,
  cancelUpload,
  completeUpload,
  fetchContents,
  lookupShare,
  putPart,
  requestTicket,
  type ShareContentsDto,
  ShareLinkError,
  type ShareLookupDto,
  shareFetch,
  type ShareUploadDto,
  type ShareUploadLimitsDto,
  startUpload,
  uploadStatus,
} from "./sharePublicApi.js";

export interface ShareNode {
  id: string;
  kind: "file" | "folder";
  name: string;
  parent: string;
  encryptedSize: number;
  meta: ShareItemMeta;
}

/** A link that's been unlocked: its decrypted tree and what it allows. */
export class OpenedShare {
  readonly nodes = new Map<string, ShareNode>();
  /** Items that didn't decrypt (damaged, or added under another key): not shown. */
  readonly unreadable: number;
  private readonly tickets = new Map<string, { ticket: string; expires: number }>();
  /** Ticket requests in flight: asking twice at once (a preview opening while a download starts) counts once. */
  private readonly pendingTickets = new Map<string, Promise<string>>();
  downloadsRemaining: number | null;

  constructor(
    readonly publicId: string,
    readonly keys: ShareKeys,
    readonly contents: ShareContentsDto,
    readonly envelope: ShareEnvelope
  ) {
    let unreadable = 0;
    for (const item of contents.items) {
      const meta = openItem(envelope.key, item.id, fromB64(item.metaCiphertext), fromB64(item.metaNonce));
      if (!meta || meta.kind !== item.kind) {
        unreadable += 1;
        continue;
      }
      this.nodes.set(item.id, { id: item.id, kind: meta.kind, name: meta.name, parent: meta.parent, encryptedSize: item.encryptedSize, meta });
    }
    this.unreadable = unreadable;
    this.downloadsRemaining = contents.downloads.remaining;
  }

  get root(): ShareNode | null {
    return this.nodes.get(this.envelope.root) ?? null;
  }

  get canUpload(): boolean {
    return this.contents.permission === "contributor" && this.contents.uploads !== null && this.envelope.upload !== null;
  }

  /** Folders first, then files, by name. Items whose folder isn't in the link show at the top. */
  children(folderId: string): ShareNode[] {
    const rootId = this.envelope.root;
    const out = [...this.nodes.values()].filter((n) => {
      if (n.id === rootId) return false;
      if (n.parent === folderId) return true;
      return folderId === rootId && !this.nodes.has(n.parent);
    });
    return out.sort((a, b) => (a.kind === b.kind ? a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: "base" }) : a.kind === "folder" ? -1 : 1));
  }

  /** From the shared root down to `id`. */
  path(id: string): ShareNode[] {
    const out: ShareNode[] = [];
    let node = this.nodes.get(id);
    for (let i = 0; node && i < 64; i += 1) {
      out.unshift(node);
      if (node.id === this.envelope.root) break;
      node = this.nodes.get(node.parent);
    }
    return out;
  }

  /** Every file under a folder, with its path inside it. */
  filesUnder(folderId: string, prefix = ""): { node: ShareNode; path: string }[] {
    const out: { node: ShareNode; path: string }[] = [];
    for (const child of this.children(folderId)) {
      if (child.kind === "file") out.push({ node: child, path: prefix + child.name });
      else out.push(...this.filesUnder(child.id, `${prefix}${child.name}/`));
    }
    return out;
  }

  /** A ticket covering these files (counts one download on the server, once). */
  async ticketFor(itemIds: string[] | "all"): Promise<string> {
    const key = itemIds === "all" ? "*" : [...itemIds].sort().join(",");
    const pending = this.pendingTickets.get(key) ?? this.pendingTickets.get("*");
    if (pending) return pending;
    const request = this.issueTicket(itemIds).finally(() => this.pendingTickets.delete(key));
    this.pendingTickets.set(key, request);
    return request;
  }

  private async issueTicket(itemIds: string[] | "all"): Promise<string> {
    const now = Date.now();
    const all = this.tickets.get("*");
    if (all && all.expires > now + 60_000) return all.ticket;
    if (itemIds !== "all" && itemIds.length === 1) {
      const one = this.tickets.get(itemIds[0]);
      if (one && one.expires > now + 60_000) return one.ticket;
    }
    const res = await requestTicket(this.publicId, this.keys.access, itemIds);
    const entry = { ticket: res.ticket, expires: Date.parse(res.expiresAt) };
    if (itemIds === "all") this.tickets.set("*", entry);
    else for (const id of itemIds) this.tickets.set(id, entry);
    this.downloadsRemaining = res.downloads.remaining;
    return res.ticket;
  }

  /** Downloads and decrypts one file; resumes by itself after a dropped connection. Never returns unverified bytes. */
  async download(node: ShareNode, options: { onProgress?: (fraction: number) => void; signal?: AbortSignal; ticket?: string } = {}): Promise<Blob> {
    if (node.meta.kind !== "file") throw new Error("Not a file");
    const meta = node.meta as ShareFileMeta;
    const ticket = options.ticket ?? (await this.ticketFor([node.id]));
    const body = resilientBody(
      (offset) =>
        shareFetch(this.publicId, `/items/${node.id}/content`, {
          headers: { ...accessHeaders(this.keys.access), "X-Share-Ticket": ticket, ...(offset > 0 ? { Range: `bytes=${offset}-` } : {}) },
          signal: options.signal,
        }),
      node.encryptedSize,
      options.signal
    );
    try {
      return await decryptFile(
        body,
        { key: fromB64(meta.key), header: fromB64(meta.header), chunkSize: meta.chunk, encryptedSize: node.encryptedSize, ciphertextSha256: meta.sha },
        "application/octet-stream",
        options.onProgress
      );
    } catch (err) {
      if (err instanceof ShareLinkError || (err as { name?: string }).name === "AbortError") throw err;
      if (err instanceof FileDecryptError) throw new ShareLinkError("This file failed its integrity check, so it wasn't saved.", "corrupt");
      throw err;
    }
  }
}

/** Step 1: what the link needs (a password?). Throws ShareLinkError for links that are gone. */
export async function lookupLink(publicId: string, secret: Uint8Array): Promise<ShareLookupDto> {
  await ready();
  return lookupShare(publicId, lookupToken(secret));
}

/** Step 2: the keys (with the password, if any), the contents, and the decrypted tree. */
export async function unlockLink(publicId: string, secret: Uint8Array, password: string | null, lookup: ShareLookupDto): Promise<OpenedShare> {
  const params = lookup.password ? { salt: fromB64(lookup.password.salt), ops: lookup.password.ops, mem: lookup.password.mem } : null;
  const keys = await deriveShareKeys(secret, params ? password : null, params);
  const contents = await fetchContents(publicId, keys.access);
  const envelope = openEnvelope(keys.wrap, fromB64(contents.envelope), fromB64(contents.envelopeNonce));
  if (!envelope) throw new ShareCryptoError("This link is damaged: it can't be opened.");
  return new OpenedShare(publicId, keys, contents, envelope);
}

const sleep = (ms: number, signal?: AbortSignal) =>
  new Promise<void>((resolve, reject) => {
    const timer = setTimeout(resolve, ms);
    signal?.addEventListener(
      "abort",
      () => {
        clearTimeout(timer);
        reject(new DOMException("Aborted", "AbortError"));
      },
      { once: true }
    );
  });

/** Waits for the browser to be back online (or a short while, in case it can't tell). */
async function waitForNetwork(attempt: number, signal?: AbortSignal): Promise<void> {
  const delay = Math.min(30_000, 1000 * 2 ** attempt);
  if (typeof navigator !== "undefined" && navigator.onLine === false) {
    await Promise.race([new Promise<void>((resolve) => window.addEventListener("online", () => resolve(), { once: true })), sleep(60_000, signal)]);
  }
  await sleep(delay, signal);
}

/** Ciphertext as it arrives; after a dropped connection, carries on from the last byte received (Range). */
export async function* resilientBody(
  open: (offset: number) => Promise<Response>,
  total: number,
  signal?: AbortSignal,
  maxRetries = 6
): AsyncGenerator<Uint8Array> {
  let offset = 0;
  let failures = 0;
  while (offset < total || total === 0) {
    let res: Response;
    try {
      res = await open(offset);
    } catch (err) {
      if (err instanceof ShareLinkError && err.transient && failures < maxRetries) {
        await waitForNetwork(failures++, signal);
        continue;
      }
      throw err;
    }
    if (offset > 0 && res.status !== 206) throw new ShareLinkError("The download couldn't resume. Start it again.", "no_resume");
    if (!res.body) throw new ShareLinkError("The server sent nothing.", "network");
    const reader = res.body.getReader();
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        if (value && value.length > 0) {
          offset += value.length;
          failures = 0;
          yield value;
        }
      }
    } catch (err) {
      if ((err as { name?: string }).name === "AbortError" || signal?.aborted) throw err;
      if (failures >= maxRetries) throw new ShareLinkError("The connection kept dropping. Try again.", "network");
      await waitForNetwork(failures++, signal);
      continue;
    } finally {
      reader.releaseLock();
    }
    if (total === 0) return;
    if (offset < total) {
      if (failures >= maxRetries) throw new ShareLinkError("The connection kept dropping. Try again.", "network");
      await waitForNetwork(failures++, signal);
    }
  }
}

// --- Uploads (Contributor links) ---

/** Names that are safe to keep: no paths, controls or direction overrides; at most 200 characters. */
export function cleanUploadName(name: string): string {
  const base = name.split(/[\\/]/).pop() ?? "";
  // eslint-disable-next-line no-control-regex
  const cleaned = base.replace(/[\u0000-\u001f\u007f‪-‮⁦-⁩]/g, "").replace(/^[.\s]+|[.\s]+$/g, "").slice(0, 200);
  return cleaned || "file";
}

export type UploadState = "waiting" | "uploading" | "paused" | "finishing" | "done" | "failed" | "cancelled";

const DEFAULT_PART_SIZE = 4 * 1024 * 1024;

export interface UploadTarget {
  publicId: string;
  access: Uint8Array;
  uploadPublicKey: Uint8Array;
  limits: ShareUploadLimitsDto;
}

/**
 * One file going to a Contributor link. Encrypted here as it's read
 * (secretstream, a fresh key, Files' format), sent in parts with their
 * SHA-256; only the part in flight is in memory. A dropped connection
 * retries by itself; after that the upload pauses and `resume()` asks the
 * server which parts arrived and carries on from there.
 */
export class ShareUploadJob {
  state: UploadState = "waiting";
  /** 0..1 of the encrypted bytes acknowledged by the server. */
  progress = 0;
  error: string | null = null;
  readonly name: string;
  /** The folder it was dropped from, if any ("Reports/2026"). */
  readonly folder: string;

  private upload: ShareUploadDto | null = null;
  private stream: { state: ReturnType<typeof sodium.crypto_secretstream_xchacha20poly1305_init_push>["state"]; header: Uint8Array; key: Uint8Array } | null = null;
  private hash: ReturnType<typeof sodium.crypto_hash_sha256_init> | null = null;
  private plainOffset = 0;
  private finished = false;
  private partNumber = 0;
  private pending: { number: number; bytes: Uint8Array; sha: string } | null = null;
  private carry = new Uint8Array(0);
  private sent = 0;
  private controller = new AbortController();
  private encryptedSize = 0;
  private partSize = DEFAULT_PART_SIZE;

  constructor(
    private readonly target: UploadTarget,
    readonly file: File,
    private readonly onChange: (job: ShareUploadJob) => void
  ) {
    this.name = cleanUploadName(file.name);
    const relative = (file as File & { webkitRelativePath?: string }).webkitRelativePath ?? "";
    this.folder = relative.includes("/") ? relative.split("/").slice(0, -1).map(cleanUploadName).join("/") : "";
  }

  /** Refuses up front what the link's limits can't take (the server checks again). */
  precheck(): string | null {
    const { limits } = this.target;
    if (limits.maxFileSize !== null && this.file.size > limits.maxFileSize) return "This file is larger than this link allows.";
    const encrypted = encryptedSizeFor(this.file.size);
    if (encrypted > limits.maxEncryptedFileSize) return "This file is larger than this link allows.";
    if (limits.remainingBytes !== null && encrypted > limits.remainingBytes) return "This file doesn't fit in what's left of this link's upload allowance.";
    if (limits.remainingFiles !== null && limits.remainingFiles < 1) return "This link has taken as many files as its owner allows.";
    return null;
  }

  async start(): Promise<void> {
    if (this.state !== "waiting") return;
    const problem = this.precheck();
    if (problem) return this.fail(problem);
    await ready();
    this.encryptedSize = encryptedSizeFor(this.file.size);
    const { minPartSize, maxPartSize } = this.target.limits;
    this.partSize = Math.min(maxPartSize, Math.max(minPartSize, DEFAULT_PART_SIZE));
    try {
      this.upload = await startUpload(this.target.publicId, this.target.access, this.encryptedSize, this.partSize, this.controller.signal);
    } catch (err) {
      return this.fail(err);
    }
    const key = sodium.crypto_secretstream_xchacha20poly1305_keygen();
    const { state, header } = sodium.crypto_secretstream_xchacha20poly1305_init_push(key);
    this.stream = { state, header, key };
    this.hash = sodium.crypto_hash_sha256_init();
    await this.run();
  }

  /** After a pause: whatever arrived stays; the rest is sent. */
  async resume(): Promise<void> {
    if (this.state !== "paused" || !this.upload) return;
    this.error = null;
    try {
      const received = await uploadStatus(this.target.publicId, this.target.access, this.upload, this.controller.signal);
      if (this.pending && received.includes(this.pending.number)) this.acknowledge();
    } catch (err) {
      if (!(err instanceof ShareLinkError && err.transient)) return this.fail(err);
    }
    await this.run();
  }

  async cancel(): Promise<void> {
    if (this.state === "done" || this.state === "cancelled") return;
    this.controller.abort();
    this.set("cancelled");
    this.wipe();
    if (this.upload) await cancelUpload(this.target.publicId, this.target.access, this.upload).catch(() => undefined);
  }

  private set(state: UploadState): void {
    this.state = state;
    this.onChange(this);
  }

  private fail(err: unknown): void {
    this.error = err instanceof Error ? err.message : String(err);
    this.wipe();
    this.set("failed");
  }

  private wipe(): void {
    if (this.stream) sodium.memzero(this.stream.key);
    this.pending = null;
  }

  private acknowledge(): void {
    if (!this.pending) return;
    this.sent += this.pending.bytes.length;
    this.progress = this.encryptedSize === 0 ? 1 : this.sent / this.encryptedSize;
    this.pending = null;
    this.onChange(this);
  }

  /** The next part of ciphertext: encrypts 64 KiB chunks of the file until a part is full (or the file ends). */
  private async nextPart(): Promise<Uint8Array | null> {
    const stream = this.stream!;
    const chunks: Uint8Array[] = this.carry.length > 0 ? [this.carry] : [];
    let length = this.carry.length;
    while (length < this.partSize && !this.finished) {
      const end = Math.min(this.plainOffset + FILE_CHUNK_BYTES, this.file.size);
      // Copied into this module's own Uint8Array: the type libsodium accepts.
      const plain = Uint8Array.from(await blobBytes(this.file.slice(this.plainOffset, end)));
      // The last chunk (an empty one for an empty file) carries TAG_FINAL, as in Files.
      const last = end >= this.file.size;
      const chunk = sodium.crypto_secretstream_xchacha20poly1305_push(
        stream.state,
        plain,
        null,
        last ? sodium.crypto_secretstream_xchacha20poly1305_TAG_FINAL : sodium.crypto_secretstream_xchacha20poly1305_TAG_MESSAGE
      );
      sodium.memzero(plain);
      sodium.crypto_hash_sha256_update(this.hash!, chunk);
      chunks.push(chunk);
      length += chunk.length;
      this.plainOffset = end;
      if (last) this.finished = true;
    }
    if (length === 0) return null;
    const all = new Uint8Array(length);
    let offset = 0;
    for (const c of chunks) {
      all.set(c, offset);
      offset += c.length;
    }
    this.carry = all.slice(Math.min(this.partSize, length));
    return all.subarray(0, Math.min(this.partSize, length));
  }

  private async sendPending(): Promise<void> {
    const pending = this.pending!;
    let failures = 0;
    for (;;) {
      try {
        await putPart(this.target.publicId, this.target.access, this.upload!, pending.number, pending.bytes, pending.sha, this.controller.signal);
        this.acknowledge();
        return;
      } catch (err) {
        if (err instanceof ShareLinkError && err.transient && failures < 6) {
          await waitForNetwork(failures++, this.controller.signal);
          continue;
        }
        throw err;
      }
    }
  }

  private async run(): Promise<void> {
    this.set("uploading");
    try {
      for (;;) {
        if (!this.pending) {
          const bytes = await this.nextPart();
          if (!bytes) break;
          this.partNumber += 1;
          this.pending = { number: this.partNumber, bytes, sha: sodium.to_hex(sodium.crypto_hash_sha256(bytes)) };
        }
        await this.sendPending();
      }
      this.set("finishing");
      const stream = this.stream!;
      const sha = sodium.to_hex(sodium.crypto_hash_sha256_final(this.hash!));
      const sealed = sealContribution(this.target.uploadPublicKey, {
        v: 1,
        name: this.folder ? `${this.folder}/${this.name}` : this.name,
        mime: this.file.type || "application/octet-stream",
        size: this.file.size,
        modified: Number.isFinite(this.file.lastModified) ? Math.max(0, Math.floor(this.file.lastModified)) : 0,
        sent: Date.now(),
        key: toB64(stream.key),
        header: toB64(stream.header),
        chunk: FILE_CHUNK_BYTES,
        sha,
      });
      sodium.memzero(stream.key);
      let failures = 0;
      for (;;) {
        try {
          await completeUpload(this.target.publicId, this.target.access, this.upload!, sha, toB64(sealed), this.controller.signal);
          break;
        } catch (err) {
          if (err instanceof ShareLinkError && err.transient && failures < 6) {
            await waitForNetwork(failures++, this.controller.signal);
            continue;
          }
          throw err;
        }
      }
      this.progress = 1;
      this.set("done");
    } catch (err) {
      if (this.state === "cancelled" || (err as { name?: string }).name === "AbortError") return;
      if (err instanceof ShareLinkError && err.transient) {
        // Out of automatic retries: keep what's encrypted and acknowledged, and wait for "Retry".
        this.error = "The connection dropped. Retry to carry on where it stopped.";
        this.set("paused");
        return;
      }
      this.fail(err);
    }
  }
}

// The web app's Files: listing, uploading, new folders and downloads, with
// the same encrypted metadata the desktop and Android apps write
// (driveMeta.ts) and the same file format (fileCrypto.ts). Every transfer
// goes through this server (proxy mode); a server set up to send people's
// browsers straight to the bucket is refused, since that would show the
// storage provider their IP address.
import * as api from "./api.js";
import { API_BASE } from "./apiBase.js";
import { decodeDriveEntry, type DriveEntry, sealDriveMeta } from "./driveMeta.js";
import { toBase64, fromBase64 } from "./e2eCrypto.js";
import { decryptFile, encryptFile, FILE_CHUNK_BYTES } from "./fileCrypto.js";

export interface DriveListing {
  entries: DriveEntry[];
  usage: api.BackupUsageDto;
}

export async function loadDrive(key: Uint8Array): Promise<DriveListing> {
  const { items, usage } = await api.listBackupItems();
  return { entries: items.map((item) => decodeDriveEntry(item, key)), usage };
}

/** The newest version of each file (what Files shows), plus folders. */
export function currentEntries(entries: DriveEntry[]): DriveEntry[] {
  const newest = new Map<string, DriveEntry>();
  const out: DriveEntry[] = [];
  for (const e of entries) {
    if (e.kind === "folder") {
      out.push(e);
      continue;
    }
    const seen = newest.get(e.fileId);
    if (!seen || e.version > seen.version || (e.version === seen.version && e.modified > seen.modified)) newest.set(e.fileId, e);
  }
  return [...out, ...newest.values()];
}

/** Every folder's id with the folders above it, for "is this inside that?". */
export function isInside(entries: Map<string, DriveEntry>, id: string, ancestor: string): boolean {
  let current = entries.get(id);
  for (let i = 0; current && i < 64; i += 1) {
    if (current.parent === ancestor) return true;
    current = entries.get(current.parent);
  }
  return false;
}

const newId = () => crypto.randomUUID();

export async function createFolder(name: string, parent: string, key: Uint8Array): Promise<DriveEntry> {
  const meta = { v: 1, kind: "folder", name, parent, modified: Date.now(), trashed: false, trashedAt: 0 };
  const sealed = sealDriveMeta(JSON.stringify(meta), key);
  const item = await api.createBackupFolder(toBase64(sealed.ciphertext), toBase64(sealed.nonce));
  return decodeDriveEntry(item, key);
}

/** Encrypts a file here and uploads the ciphertext into Files under `parent`. */
export async function uploadFile(
  file: File,
  parent: string,
  key: Uint8Array,
  onProgress?: (fraction: number) => void,
  name = file.name
): Promise<void> {
  const encrypted = await encryptFile(file, (p) => onProgress?.(p * 0.3));
  const meta = {
    v: 1,
    kind: "file",
    name,
    parent,
    modified: Date.now(),
    trashed: false,
    trashedAt: 0,
    mime: file.type || "application/octet-stream",
    size: file.size,
    fileId: newId(),
    version: 1,
    key: toBase64(encrypted.key),
    header: toBase64(encrypted.header),
    chunk: encrypted.chunkSize,
    sha: encrypted.ciphertextSha256,
  };
  const sealed = sealDriveMeta(JSON.stringify(meta), key);
  const { objectId, upload } = await api.requestBackupUpload({
    encryptedSize: encrypted.encryptedSize,
    ciphertextSha256: encrypted.ciphertextSha256,
    metaCiphertext: toBase64(sealed.ciphertext),
    metaNonce: toBase64(sealed.nonce),
  });
  if (upload.mode !== "proxy" || !upload.url.startsWith("/backup/")) {
    throw new Error("This server sends uploads straight to its storage provider, which this page doesn't do. Use the desktop or Android app.");
  }
  await new Promise<void>((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("PUT", `${API_BASE}${upload.url}`);
    xhr.withCredentials = true;
    for (const [name, value] of Object.entries(upload.headers)) xhr.setRequestHeader(name, value);
    xhr.upload.onprogress = (e) => e.lengthComputable && onProgress?.(0.3 + 0.7 * (e.loaded / e.total));
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) resolve();
      else {
        let message = "The upload failed. Try again.";
        try {
          message = (JSON.parse(xhr.responseText) as { message?: string }).message ?? message;
        } catch {
          // keep the generic message
        }
        reject(new Error(message));
      }
    };
    xhr.onerror = () => reject(new Error("The connection dropped. Try again."));
    xhr.send(encrypted.blob);
  });
  await api.completeBackupUpload(objectId).catch(() => undefined);
  onProgress?.(1);
}

/** Downloads one of your own files and decrypts it here (checked against its hash before anything is returned). */
export async function downloadFile(entry: DriveEntry, onProgress?: (fraction: number) => void): Promise<Blob> {
  const meta = entry.meta ?? {};
  if (entry.locked || typeof meta.key !== "string" || typeof meta.header !== "string" || typeof meta.sha !== "string") {
    throw new Error("This file is locked with its own key or password. Unlock it in the desktop or Android app.");
  }
  const { download } = await api.backupDownloadTarget(entry.id);
  if (download.mode !== "proxy" || !download.url.startsWith("/backup/")) {
    throw new Error("This server sends downloads straight from its storage provider, which this page doesn't do.");
  }
  const res = await fetch(`${API_BASE}${download.url}`, { credentials: "include", cache: "no-store" });
  if (!res.ok || !res.body) throw new Error("Couldn't download the file. Try again.");
  return decryptFile(
    readStream(res.body),
    {
      key: fromBase64(meta.key),
      header: fromBase64(meta.header),
      chunkSize: typeof meta.chunk === "number" ? meta.chunk : FILE_CHUNK_BYTES,
      encryptedSize: entry.storedBytes,
      ciphertextSha256: meta.sha,
    },
    "application/octet-stream",
    onProgress
  );
}

export async function* readStream(body: ReadableStream<Uint8Array>): AsyncGenerator<Uint8Array> {
  const reader = body.getReader();
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) return;
      if (value) yield value;
    }
  } finally {
    reader.releaseLock();
  }
}

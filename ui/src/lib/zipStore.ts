// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// "Download all": already-decrypted files packed into one .zip, here in the
// browser (stored, not compressed: most shared files are compressed
// already, and it keeps memory flat). Plain ZIP without ZIP64, so up to
// 65,535 files and 4 GB in total; bigger folders download file by file.
import { blobBytes } from "./blobBytes.js";
import { safeFileName } from "./safePreview.js";

export const ZIP_MAX_BYTES = 0xffffffff - 64 * 1024 * 1024;
export const ZIP_MAX_FILES = 65_000;

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n += 1) {
    let c = n;
    for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

export function crc32(bytes: Uint8Array, crc = 0): number {
  let c = ~crc >>> 0;
  for (let i = 0; i < bytes.length; i += 1) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

async function crcOfBlob(blob: Blob): Promise<number> {
  let crc = 0;
  const step = 4 * 1024 * 1024;
  for (let offset = 0; offset < blob.size; offset += step) {
    crc = crc32(await blobBytes(blob.slice(offset, offset + step)), crc);
  }
  return crc;
}

function dosTime(ms: number): { time: number; date: number } {
  const d = new Date(ms > 0 ? ms : Date.now());
  const year = Math.max(1980, d.getFullYear());
  return {
    time: (d.getHours() << 11) | (d.getMinutes() << 5) | Math.floor(d.getSeconds() / 2),
    date: ((year - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate(),
  };
}

/** A path inside the archive: every part made safe, no "..", no leading slash. */
export function archivePath(path: string): string {
  return path
    .split("/")
    .filter((part) => part !== "" && part !== "." && part !== "..")
    .map(safeFileName)
    .join("/");
}

export interface ZipEntry {
  path: string;
  data: Blob;
  modified: number;
}

/** Builds the archive as a Blob (its parts are the files' own Blobs, so nothing is copied twice). */
export async function buildZip(entries: ZipEntry[]): Promise<Blob> {
  if (entries.length > ZIP_MAX_FILES) throw new Error("Too many files for one archive. Download them separately.");
  const total = entries.reduce((n, e) => n + e.data.size, 0);
  if (total > ZIP_MAX_BYTES) throw new Error("These files are too big for one archive. Download them separately.");
  const encoder = new TextEncoder();
  const parts: BlobPart[] = [];
  const central: Uint8Array[] = [];
  const used = new Set<string>();
  let offset = 0;
  for (const entry of entries) {
    let path = archivePath(entry.path) || "file";
    for (let n = 2; used.has(path.toLowerCase()); n += 1) {
      const dot = path.lastIndexOf(".");
      path = dot > path.lastIndexOf("/") + 1 ? `${path.slice(0, dot)} (${n})${path.slice(dot)}` : `${path} (${n})`;
    }
    used.add(path.toLowerCase());
    const name = encoder.encode(path);
    const crc = await crcOfBlob(entry.data);
    const { time, date } = dosTime(entry.modified);
    const size = entry.data.size;

    const local = new DataView(new ArrayBuffer(30));
    local.setUint32(0, 0x04034b50, true);
    local.setUint16(4, 20, true);
    local.setUint16(6, 0x0800, true); // UTF-8 names
    local.setUint16(8, 0, true); // stored
    local.setUint16(10, time, true);
    local.setUint16(12, date, true);
    local.setUint32(14, crc, true);
    local.setUint32(18, size, true);
    local.setUint32(22, size, true);
    local.setUint16(26, name.length, true);
    local.setUint16(28, 0, true);
    parts.push(local.buffer, name as BlobPart, entry.data);

    const header = new DataView(new ArrayBuffer(46));
    header.setUint32(0, 0x02014b50, true);
    header.setUint16(4, 20, true);
    header.setUint16(6, 20, true);
    header.setUint16(8, 0x0800, true);
    header.setUint16(10, 0, true);
    header.setUint16(12, time, true);
    header.setUint16(14, date, true);
    header.setUint32(16, crc, true);
    header.setUint32(20, size, true);
    header.setUint32(24, size, true);
    header.setUint16(28, name.length, true);
    header.setUint32(42, offset, true);
    const record = new Uint8Array(46 + name.length);
    record.set(new Uint8Array(header.buffer));
    record.set(name, 46);
    central.push(record);
    offset += 30 + name.length + size;
  }
  const centralSize = central.reduce((n, c) => n + c.length, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true);
  end.setUint16(8, entries.length, true);
  end.setUint16(10, entries.length, true);
  end.setUint32(12, centralSize, true);
  end.setUint32(16, offset, true);
  return new Blob([...parts, ...(central as BlobPart[]), end.buffer], { type: "application/zip" });
}

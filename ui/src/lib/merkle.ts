// Verifiers for the transparency log's Merkle proofs: a port of
// server/src/crypto/merkle.ts (RFC 9162), run here so this device checks
// the log itself instead of taking the server's word for it. Hashing uses
// WebCrypto (libsodium's sha256 rejects byte arrays built in plain JS; see
// e2eCrypto.safetyNumberDigits).

export function hexToBytes(hex: string): Uint8Array {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}

export function bytesToHex(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

async function sha256(...parts: Uint8Array[]): Promise<Uint8Array> {
  const total = parts.reduce((n, p) => n + p.length, 0);
  const joined = new Uint8Array(total);
  let offset = 0;
  for (const p of parts) {
    joined.set(p, offset);
    offset += p.length;
  }
  return new Uint8Array(await crypto.subtle.digest("SHA-256", joined));
}

export function leafHash(data: Uint8Array): Promise<Uint8Array> {
  return sha256(new Uint8Array([0]), data);
}

function nodeHash(left: Uint8Array, right: Uint8Array): Promise<Uint8Array> {
  return sha256(new Uint8Array([1]), left, right);
}

function equal(a: Uint8Array, b: Uint8Array): boolean {
  return a.length === b.length && a.every((x, i) => x === b[i]);
}

/** RFC 9162 2.1.3.2 */
export async function verifyInclusion(leaf: Uint8Array, index: number, size: number, proof: Uint8Array[], root: Uint8Array): Promise<boolean> {
  if (index >= size) return false;
  let fn = index;
  let sn = size - 1;
  let r = leaf;
  for (const p of proof) {
    if (sn === 0) return false;
    if (fn % 2 === 1 || fn === sn) {
      r = await nodeHash(p, r);
      while (fn % 2 === 0 && fn !== 0) {
        fn = Math.floor(fn / 2);
        sn = Math.floor(sn / 2);
      }
    } else {
      r = await nodeHash(r, p);
    }
    fn = Math.floor(fn / 2);
    sn = Math.floor(sn / 2);
  }
  return sn === 0 && equal(r, root);
}

/** RFC 9162 2.1.4.2 */
export async function verifyConsistency(from: number, to: number, fromRoot: Uint8Array, toRoot: Uint8Array, proof: Uint8Array[]): Promise<boolean> {
  if (from < 1 || from > to) return false;
  if (from === to) return proof.length === 0 && equal(fromRoot, toRoot);
  const path = (from & (from - 1)) === 0 ? [fromRoot, ...proof] : [...proof];
  if (path.length === 0) return false;
  let fn = from - 1;
  let sn = to - 1;
  while (fn % 2 === 1) {
    fn = Math.floor(fn / 2);
    sn = Math.floor(sn / 2);
  }
  let fr = path[0];
  let sr = path[0];
  for (const c of path.slice(1)) {
    if (sn === 0) return false;
    if (fn % 2 === 1 || fn === sn) {
      fr = await nodeHash(c, fr);
      sr = await nodeHash(c, sr);
      while (fn % 2 === 0 && fn !== 0) {
        fn = Math.floor(fn / 2);
        sn = Math.floor(sn / 2);
      }
    } else {
      sr = await nodeHash(sr, c);
    }
    fn = Math.floor(fn / 2);
    sn = Math.floor(sn / 2);
  }
  return sn === 0 && equal(fr, fromRoot) && equal(sr, toRoot);
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The password manager, compatible with the desktop app's vault
// (desktop/core/src/vault_crypto.cpp, data/vault_repository.cpp). Every item
// is a JSON object, padded with spaces to a 256-byte bucket and sealed with a
// key derived from the account's private key, so the server only ever holds
// ciphertext. The same item opens on the desktop and here.
import { hmac } from "@noble/hashes/hmac.js";
import { sha1 } from "@noble/hashes/legacy.js";
import { sha256, sha512 } from "@noble/hashes/sha2.js";
import sodium from "libsodium-wrappers-sumo";
import { decryptWithConversationKey, encryptWithConversationKey, fromBase64, toBase64, unwrapPrivateKey, type WrappedPrivateKey } from "./e2eCrypto.js";
import { WORDS } from "./sas.js";

const PAD_BUCKET = 256;

export type VaultItemType = "login" | "passkey" | "card" | "note" | "wifi";

/** An item's decrypted data, as the desktop writes it. Unknown fields are kept as they are. */
export interface VaultData {
  v?: number;
  type?: string;
  title?: string;
  username?: string;
  password?: string;
  urls?: string[];
  notes?: string;
  totp?: string;
  card?: { holder?: string; number?: string; expiry?: string; cvv?: string };
  wifi?: { ssid?: string; security?: string; password?: string };
  passkey?: { rpId?: string; userName?: string };
  created?: number;
  updated?: number;
  passwordChanged?: number;
  /** The record this item belongs to, inside the sealed data, so a server can't swap two records. */
  boundId?: string;
  /** Goes up with each save. A device refuses anything lower than it has seen, so it can't be handed an older copy. */
  rev?: number;
  [key: string]: unknown;
}

export interface VaultEntry {
  id: string;
  revision: number;
  data: VaultData;
  /** Shared with you by someone else: read-only. */
  incoming: boolean;
  ownerId: string | null;
  /** Your own items: who they're shared with (user ids). */
  sharedWith: string[];
}

// --- Binding items to their records ---

const REV_PREFIX = "redvaultsystem:vault-rev:";
const revsName = (userId: string) => REV_PREFIX + userId.toLowerCase();

function loadRevs(userId: string): Record<string, number> {
  try {
    return JSON.parse(localStorage.getItem(revsName(userId)) ?? "{}") as Record<string, number>;
  } catch {
    return {};
  }
}

/** False for an item in another record's place, or older than this device has seen. Items are bound when next saved. */
export function acceptItem(userId: string, id: string, data: VaultData, slot: "own" | "shared" = "own"): boolean {
  const record = id.toLowerCase();
  if (data.boundId !== undefined && data.boundId.toLowerCase() !== record) return false;
  const revs = loadRevs(userId);
  const name = slot === "own" ? record : `shared:${record}`;
  const rev = typeof data.rev === "number" ? data.rev : 0;
  if (rev < (revs[name] ?? 0)) return false;
  if (rev > (revs[name] ?? 0)) {
    try {
      localStorage.setItem(revsName(userId), JSON.stringify({ ...revs, [name]: rev }));
    } catch {
      // Private browsing or disabled storage: there's then nothing to compare with next time.
    }
  }
  return true;
}

// --- Keys and sealing ---

/** Same derivation as the desktop's crypto::vault::vaultKey. */
export function vaultKey(privateKey: Uint8Array): Uint8Array {
  return sodium.crypto_generichash(32, "redvault-password-vault-v1", privateKey);
}

function padded(json: string): string {
  const bytes = new TextEncoder().encode(json).length;
  return json + " ".repeat((Math.floor(bytes / PAD_BUCKET) + 1) * PAD_BUCKET - bytes);
}

export function sealItem(data: VaultData, key: Uint8Array): { ciphertext: string; nonce: string } {
  const { ciphertext, nonce } = encryptWithConversationKey(padded(JSON.stringify(data)), key);
  return { ciphertext: toBase64(ciphertext), nonce: toBase64(nonce) };
}

export function openItem(ciphertext: string, nonce: string, key: Uint8Array): VaultData {
  return JSON.parse(decryptWithConversationKey(fromBase64(ciphertext), fromBase64(nonce), key).replace(/ +$/, "")) as VaultData;
}

/** A read-only copy only the recipient can open (crypto_box_seal to their public key). */
export function sealForRecipient(data: VaultData, recipientPublicKey: Uint8Array): string {
  return toBase64(sodium.crypto_box_seal(padded(JSON.stringify(data)), recipientPublicKey));
}

export function openFromSender(sealed: string, publicKey: Uint8Array, privateKey: Uint8Array): VaultData {
  const plain = sodium.crypto_box_seal_open(fromBase64(sealed), publicKey, privateKey, "text");
  return JSON.parse(plain.replace(/ +$/, "")) as VaultData;
}

/** Encryption keys for Files (desktop key manager) live in the vault too, but never on the Passwords page. */
export function isHiddenItem(data: VaultData): boolean {
  return data.type === "filekey";
}

// --- Unlocking with the account password ---

// The password-wrapped private key from sign-in, kept in memory only, so
// the vault can be unlocked by re-entering the account password without
// asking the server anything. Cleared on sign-out.
let wrappedForUnlock: WrappedPrivateKey | null = null;

export function rememberWrappedKey(wrapped: WrappedPrivateKey | null): void {
  wrappedForUnlock = wrapped;
}

export function canUnlockWithPassword(): boolean {
  return wrappedForUnlock !== null;
}

/** True if `password` is this account's password (it unwraps to the same private key). */
export function checkAccountPassword(password: string, privateKey: Uint8Array): boolean {
  if (!wrappedForUnlock || !password) return false;
  try {
    const unwrapped = unwrapPrivateKey(wrappedForUnlock, password);
    return unwrapped.length === privateKey.length && sodium.memcmp(unwrapped, privateKey);
  } catch {
    return false;
  }
}

// --- Item helpers ---

export function secretOf(data: VaultData): string {
  return data.type === "wifi" ? (data.wifi?.password ?? "") : (data.password ?? "");
}

export function firstUrl(data: VaultData): string {
  return Array.isArray(data.urls) && typeof data.urls[0] === "string" ? data.urls[0] : "";
}

export function hostOf(url: string): string {
  if (!url) return "";
  try {
    return new URL(/^[a-z][a-z0-9+.-]*:\/\//i.test(url) ? url : `https://${url}`).host || url;
  } catch {
    return url;
  }
}

// --- Strength (the desktop's estimateStrength, rule for rule) ---

const COMMON = new Set([
  "123456", "123456789", "12345678", "password", "qwerty", "qwerty123", "1q2w3e4r", "12345", "111111", "123123",
  "1234567890", "1234567", "abc123", "password1", "password123", "iloveyou", "admin", "welcome", "monkey", "dragon",
  "letmein", "football", "baseball", "sunshine", "princess", "000000", "654321", "superman", "qazwsx", "trustno1",
  "master", "hello", "freedom", "whatever", "shadow", "michael", "charlie", "passw0rd", "p@ssw0rd", "zaq12wsx",
  "asdfghjkl", "qwertyuiop", "starwars", "login", "welcome1", "secret", "changeme", "default", "root", "test",
]);

export type Strength = "Strong" | "Fair" | "Weak";

export function estimateStrength(password: string): { strength: Strength; bits: number } {
  const finish = (bits: number) => ({ bits, strength: (bits >= 75 ? "Strong" : bits >= 50 ? "Fair" : "Weak") as Strength });
  if (!password) return finish(0);
  // The desktop works on UTF-8 bytes; so does this.
  const bytes = new TextEncoder().encode(password);
  const lower = password.toLowerCase();
  if (COMMON.has(lower)) return finish(8);
  let hasLower = false;
  let hasUpper = false;
  let hasDigit = false;
  let hasSymbol = false;
  let hasOther = false;
  for (const c of bytes) {
    if (c >= 97 && c <= 122) hasLower = true;
    else if (c >= 65 && c <= 90) hasUpper = true;
    else if (c >= 48 && c <= 57) hasDigit = true;
    else if (c < 128) hasSymbol = true;
    else hasOther = true;
  }
  const pool = (hasLower ? 26 : 0) + (hasUpper ? 26 : 0) + (hasDigit ? 10 : 0) + (hasSymbol ? 33 : 0) + (hasOther ? 64 : 0);
  let effective = 1;
  for (let i = 1; i < bytes.length; i++) {
    const a = bytes[i - 1];
    const b = bytes[i];
    if (a === b) effective += 0.2;
    else if (b === a + 1 || b + 1 === a) effective += 0.35;
    else effective += 1;
  }
  const log = Math.log2(Math.max(pool, 2));
  let bits = effective * log;
  for (const common of COMMON) {
    if (common.length >= 5 && lower.includes(common)) {
      bits -= common.length * log * 0.8;
      break;
    }
  }
  return finish(Math.max(bits, 0));
}

// --- Generator (the desktop's generatePassword) ---

export interface GeneratorOptions {
  length: number;
  upper: boolean;
  digits: boolean;
  symbols: boolean;
  words: boolean;
}

export const DEFAULT_GENERATOR: GeneratorOptions = { length: 24, upper: true, digits: true, symbols: true, words: false };

export function generatePassword(options: GeneratorOptions): string {
  const pick = (n: number) => sodium.randombytes_uniform(n);
  if (options.words) {
    const count = Math.min(12, Math.max(4, options.length));
    const words: string[] = [];
    for (let i = 0; i < count; i++) {
      let word = WORDS[pick(256)];
      if (options.upper && i === 0) word = word[0].toUpperCase() + word.slice(1);
      words.push(word);
    }
    return words.join("-") + (options.digits ? `-${pick(100)}` : "");
  }
  const sets = ["abcdefghijkmnopqrstuvwxyz"]; // no l: it reads like 1 and I
  if (options.upper) sets.push("ABCDEFGHJKLMNPQRSTUVWXYZ"); // no I or O
  if (options.digits) sets.push("23456789"); // no 0 or 1
  if (options.symbols) sets.push("!@#$%^&*-_=+?");
  const all = sets.join("");
  const length = Math.min(128, Math.max(8, options.length));
  // Redraw until every chosen class appears (rarely needs a second try).
  for (;;) {
    let out = "";
    for (let i = 0; i < length; i++) out += all[pick(all.length)];
    if (sets.every((set) => [...out].some((c) => set.includes(c)))) return out;
  }
}

// --- One-time codes (RFC 6238), as the desktop parses them ---

export interface TotpParams {
  secret: Uint8Array;
  algorithm: "SHA1" | "SHA256" | "SHA512";
  digits: number;
  period: number;
  issuer: string;
  account: string;
}

export function base32Decode(text: string): Uint8Array | null {
  const out: number[] = [];
  let buffer = 0;
  let bits = 0;
  let any = false;
  for (const raw of text) {
    if (raw === " " || raw === "-" || raw === "=") continue;
    const c = raw.toUpperCase();
    let value: number;
    if (c >= "A" && c <= "Z") value = c.charCodeAt(0) - 65;
    else if (c >= "2" && c <= "7") value = c.charCodeAt(0) - 50 + 26;
    else return null;
    any = true;
    buffer = ((buffer << 5) | value) & 0xffff;
    bits += 5;
    if (bits >= 8) {
      out.push((buffer >> (bits - 8)) & 0xff);
      bits -= 8;
    }
  }
  return any && out.length ? new Uint8Array(out) : null;
}

export function parseTotp(input: string): TotpParams | null {
  const text = input.trim();
  const params: TotpParams = { secret: new Uint8Array(), algorithm: "SHA1", digits: 6, period: 30, issuer: "", account: "" };
  if (text.startsWith("otpauth://")) {
    let url: URL;
    try {
      url = new URL(text);
    } catch {
      return null;
    }
    if (url.host.toLowerCase() !== "totp") return null;
    const label = decodeURIComponent(url.pathname.replace(/^\//, ""));
    const colon = label.indexOf(":");
    if (colon >= 0) {
      params.issuer = label.slice(0, colon);
      params.account = label.slice(colon + 1).trimStart();
    } else {
      params.account = label;
    }
    const query = new Map([...url.searchParams].map(([k, v]) => [k.toLowerCase(), v]));
    if (query.get("issuer")) params.issuer = query.get("issuer")!;
    const algorithm = (query.get("algorithm") ?? "SHA1").toUpperCase();
    if (algorithm !== "SHA1" && algorithm !== "SHA256" && algorithm !== "SHA512") return null;
    params.algorithm = algorithm;
    const digits = Number.parseInt(query.get("digits") ?? "6", 10);
    const period = Number.parseInt(query.get("period") ?? "30", 10);
    params.digits = digits >= 6 && digits <= 8 ? digits : 6;
    params.period = period >= 1 && period <= 300 ? period : 30;
    const secret = base32Decode(query.get("secret") ?? "");
    if (!secret) return null;
    params.secret = secret;
  } else {
    const secret = base32Decode(text);
    if (!secret) return null;
    params.secret = secret;
  }
  return params;
}

export function totpCode(params: TotpParams, unixSeconds: number): string {
  let counter = Math.floor(Math.max(0, unixSeconds) / params.period);
  const message = new Uint8Array(8);
  for (let i = 7; i >= 0; i--) {
    message[i] = counter % 256;
    counter = Math.floor(counter / 256);
  }
  const hash = params.algorithm === "SHA256" ? sha256 : params.algorithm === "SHA512" ? sha512 : sha1;
  const mac = hmac(hash, params.secret, message);
  const offset = mac[mac.length - 1] & 0x0f;
  const binary = ((mac[offset] & 0x7f) << 24) | (mac[offset + 1] << 16) | (mac[offset + 2] << 8) | mac[offset + 3];
  return String((binary >>> 0) % 10 ** params.digits).padStart(params.digits, "0");
}

export function totpSecondsLeft(params: TotpParams, unixSeconds: number): number {
  return params.period - (Math.floor(unixSeconds) % params.period);
}

// --- Breach check (k-anonymity: only a 5-character SHA-1 prefix leaves this device) ---

export function breachHash(password: string): string {
  return Array.from(sha1(new TextEncoder().encode(password)), (b) => b.toString(16).padStart(2, "0"))
    .join("")
    .toUpperCase();
}

export function breachCount(hashHex: string, rangeResponse: string): number {
  if (hashHex.length !== 40) return 0;
  const suffix = hashHex.slice(5);
  for (const raw of rangeResponse.split("\n")) {
    const line = raw.replace(/\r$/, "");
    if (line.length > 36 && line[35] === ":" && line.slice(0, 35) === suffix) return Number.parseInt(line.slice(36), 10) || 0;
  }
  return 0;
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import sodium from "libsodium-wrappers-sumo";
import { beforeAll, describe, expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import {
  base32Decode,
  breachCount,
  breachHash,
  checkAccountPassword,
  estimateStrength,
  generatePassword,
  isHiddenItem,
  openFromSender,
  openItem,
  parseTotp,
  rememberWrappedKey,
  sealForRecipient,
  sealItem,
  totpCode,
  totpSecondsLeft,
  vaultKey,
} from "../lib/passwordVault.js";
import { WORDS } from "../lib/sas.js";

beforeAll(async () => {
  await crypto.ready();
});

function base32(bytes: Uint8Array): string {
  const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  let bits = 0;
  let value = 0;
  let out = "";
  for (const b of bytes) {
    value = (value << 8) | b;
    bits += 8;
    while (bits >= 5) {
      out += alphabet[(value >>> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) out += alphabet[(value << (5 - bits)) & 31];
  return out;
}

describe("vault items (desktop format)", () => {
  it("opens an item sealed exactly the way the desktop app seals it", () => {
    const { privateKey } = crypto.generateKeyPair();
    // desktop/core/src/vault_crypto.cpp: key = BLAKE2b-256(key=private key, "redvault-password-vault-v1"),
    // JSON padded with spaces to a 256-byte bucket, XChaCha20-Poly1305 with no associated data.
    const key = sodium.crypto_generichash(32, "redvault-password-vault-v1", privateKey);
    const json = JSON.stringify({ v: 1, type: "login", title: "Cloud console", username: "admin@example.com", password: "Qp8#nZ2!wR6@", urls: ["https://console.example.net"], extra: { kept: true } });
    const plain = json + " ".repeat(Math.ceil((json.length + 1) / 256) * 256 - json.length);
    const nonce = new Uint8Array(24).fill(3);
    const ciphertext = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(plain, null, null, nonce, key);
    const data = openItem(crypto.toBase64(ciphertext), crypto.toBase64(nonce), vaultKey(privateKey));
    expect(data).toEqual({ v: 1, type: "login", title: "Cloud console", username: "admin@example.com", password: "Qp8#nZ2!wR6@", urls: ["https://console.example.net"], extra: { kept: true } });
  });

  it("seals items the desktop can open: padded to 256 bytes, same key", () => {
    const { privateKey } = crypto.generateKeyPair();
    const sealed = sealItem({ type: "note", title: "Wi-Fi at mum's", notes: "ünïcödé ✓" }, vaultKey(privateKey));
    const key = sodium.crypto_generichash(32, "redvault-password-vault-v1", privateKey);
    const plain = sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(null, crypto.fromBase64(sealed.ciphertext), null, crypto.fromBase64(sealed.nonce), key);
    expect(plain.length % 256).toBe(0);
    expect(JSON.parse(new TextDecoder().decode(plain).trimEnd())).toEqual({ type: "note", title: "Wi-Fi at mum's", notes: "ünïcödé ✓" });
  });

  it("shares a read-only copy only the recipient can open", () => {
    const bob = crypto.generateKeyPair();
    const eve = crypto.generateKeyPair();
    const sealed = sealForRecipient({ type: "login", title: "Shared", password: "x" }, bob.publicKey);
    expect(openFromSender(sealed, bob.publicKey, bob.privateKey)).toEqual({ type: "login", title: "Shared", password: "x" });
    expect(() => openFromSender(sealed, eve.publicKey, eve.privateKey)).toThrow();
  });

  it("keeps Files encryption keys off the Passwords page", () => {
    expect(isHiddenItem({ type: "filekey", title: "Tax archive key" })).toBe(true);
    expect(isHiddenItem({ type: "login" })).toBe(false);
  });

  it("unlocks only with the account password, checked on this device", () => {
    const { privateKey } = crypto.generateKeyPair();
    rememberWrappedKey(null);
    expect(checkAccountPassword("correct horse battery staple", privateKey)).toBe(false);
    rememberWrappedKey(crypto.wrapPrivateKey(privateKey, "correct horse battery staple"));
    expect(checkAccountPassword("correct horse battery staple", privateKey)).toBe(true);
    expect(checkAccountPassword("wrong", privateKey)).toBe(false);
    expect(checkAccountPassword("correct horse battery staple", crypto.generateKeyPair().privateKey)).toBe(false);
    rememberWrappedKey(null);
  });
});

describe("one-time codes (RFC 6238 test vectors)", () => {
  const ascii = (s: string) => new TextEncoder().encode(s);
  const seed20 = base32(ascii("12345678901234567890"));
  const seed32 = base32(ascii("12345678901234567890123456789012"));
  const seed64 = base32(ascii("1234567890123456789012345678901234567890123456789012345678901234"));

  it.each([
    [59, "SHA1", seed20, "94287082"],
    [1111111109, "SHA1", seed20, "07081804"],
    [2000000000, "SHA1", seed20, "69279037"],
    [59, "SHA256", seed32, "46119246"],
    [1111111111, "SHA256", seed32, "67062674"],
    [59, "SHA512", seed64, "90693936"],
    [20000000000, "SHA512", seed64, "47863826"],
  ])("t=%s %s", (t, algorithm, seed, code) => {
    const params = parseTotp(`otpauth://totp/Example:alice@example.com?secret=${seed}&algorithm=${algorithm}&digits=8&issuer=Example`)!;
    expect(params).toMatchObject({ algorithm, digits: 8, period: 30, issuer: "Example", account: "alice@example.com" });
    expect(totpCode(params, t)).toBe(code);
  });

  it("accepts a bare key, like the desktop, and refuses junk", () => {
    const params = parseTotp("gezd gnbv gy3t qojq gezd gnbv gy3t qojq")!;
    expect(params).toMatchObject({ algorithm: "SHA1", digits: 6, period: 30 });
    expect(totpCode(params, 59)).toBe("287082");
    expect(totpSecondsLeft(params, 59)).toBe(1);
    expect(parseTotp("not a key!")).toBeNull();
    expect(parseTotp("otpauth://hotp/x?secret=GEZDGNBV")).toBeNull();
    expect(parseTotp("otpauth://totp/x?secret=GEZDGNBV&algorithm=MD5")).toBeNull();
    expect(base32Decode("")).toBeNull();
  });
});

describe("private breach check", () => {
  it("sends only a 5-character prefix and finds the count in the range", () => {
    const hash = breachHash("password");
    expect(hash).toBe("5BAA61E4C9B93F3F0682250B6CF8331B7EE68FD8");
    const range = "0018A45C4D1DEF81644B54AB7F969B88D65:1\r\n1E4C9B93F3F0682250B6CF8331B7EE68FD8:10434004\r\n011053FD0102E94D6AE2F8B83D76FAF94F6:3";
    expect(breachCount(hash, range)).toBe(10434004);
    expect(breachCount(breachHash("a-strong-unique-passphrase"), range)).toBe(0);
  });
});

describe("strength and the generator (same rules as the desktop)", () => {
  it("rates passwords like the desktop", () => {
    expect(estimateStrength("password")).toMatchObject({ strength: "Weak", bits: 8 });
    expect(estimateStrength("aaaaaaaaaaaa").strength).toBe("Weak");
    expect(estimateStrength("mypassword2024!").strength).toBe("Weak");
    expect(estimateStrength("bluesky2020").strength).toBe("Fair");
    expect(estimateStrength("correcthorsebattery").strength).toBe("Strong");
    expect(estimateStrength("Qp8#nZ2!wR6@yK4$tM9&vL3x").strength).toBe("Strong");
  });

  it("generates passwords with every chosen kind of character and no look-alikes", () => {
    for (let i = 0; i < 50; i++) {
      const p = generatePassword({ length: 24, upper: true, digits: true, symbols: true, words: false });
      expect(p).toHaveLength(24);
      expect(p).toMatch(/[a-z]/);
      expect(p).toMatch(/[A-Z]/);
      expect(p).toMatch(/[2-9]/);
      expect(p).toMatch(/[!@#$%^&*\-_=+?]/);
      expect(p).not.toMatch(/[lIO01]/);
      expect(estimateStrength(p).strength).toBe("Strong");
    }
    expect(generatePassword({ length: 3, upper: false, digits: false, symbols: false, words: false })).toMatch(/^[a-z]{8}$/);
  });

  it("makes word passwords from the shared word list", () => {
    const p = generatePassword({ length: 6, upper: true, digits: true, symbols: false, words: true });
    const parts = p.split("-");
    expect(parts).toHaveLength(7);
    expect(parts[6]).toMatch(/^\d{1,2}$/);
    expect(WORDS).toContain(parts[0].toLowerCase());
    expect(parts[0][0]).toBe(parts[0][0].toUpperCase());
    parts.slice(1, 6).forEach((w) => expect(WORDS).toContain(w));
  });
});

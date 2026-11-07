// Call links: a random 32-byte secret per link, carried in the link's URL
// fragment (#…), which browsers never send to any server. The link's name
// and the call's offer and answer are encrypted and authenticated with it,
// so only people holding the link can read the name or set up the call,
// and the server can't slip into the middle of one.
//
// The owner keeps the secret, sealed to their own public key, on the
// server, so any of their devices can show and copy the link again.
import sodium from "libsodium-wrappers-sumo";
import type { CallLinkDto, SealedSignalDto } from "./api.js";
import { API_BASE } from "./apiBase.js";
import { CallSignalError, type CallSignal } from "./callCrypto.js";
import { decryptWithConversationKey, encryptWithConversationKey, fromBase64, toBase64 } from "./e2eCrypto.js";
import { pastedUrl } from "./pastedUrl.js";

export interface OpenedCallLink {
  id: string;
  kind: "voice" | "video";
  name: string;
  secret: Uint8Array;
  url: string;
  createdOn: string;
}

function toBase64Url(bytes: Uint8Array): string {
  return sodium.to_base64(bytes, sodium.base64_variants.URLSAFE_NO_PADDING);
}

export function secretFromFragment(fragment: string): Uint8Array | null {
  try {
    const bytes = sodium.from_base64(fragment.replace(/^#/, ""), sodium.base64_variants.URLSAFE_NO_PADDING);
    return bytes.length === 32 ? bytes : null;
  } catch {
    return null;
  }
}

/** The shareable URL: this app's address, the link id, and the secret in the fragment. */
export function callLinkUrl(id: string, secret: Uint8Array): string {
  const base = new URL(API_BASE.replace(/\/api\/?$/, "/") || "/", window.location.origin);
  base.search = `?join=${encodeURIComponent(id)}`;
  base.hash = toBase64Url(secret);
  return base.toString();
}

/**
 * What someone pasted to join a call: a whole call link (…?join=<id>#<secret>).
 * Only checks the shape (an id, and a secret of 32 bytes in base64url); the
 * join screen checks the secret against the link's encrypted name.
 */
export function parseCallLink(text: string): { linkId: string; secret: string } | null {
  const url = pastedUrl(text);
  if (!url) return null;
  const linkId = url.searchParams.get("join") ?? "";
  const secret = url.hash.replace(/^#/, "");
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(linkId) || !/^[A-Za-z0-9_-]{43}$/.test(secret)) return null;
  return { linkId, secret };
}

export function newCallLinkSecret(): Uint8Array {
  return sodium.randombytes_buf(32);
}

export function sealLinkName(name: string, secret: Uint8Array): { nameCiphertext: string; nameNonce: string } {
  const { ciphertext, nonce } = encryptWithConversationKey(name, secret);
  return { nameCiphertext: toBase64(ciphertext), nameNonce: toBase64(nonce) };
}

export function openLinkName(nameCiphertext: string, nameNonce: string, secret: Uint8Array): string {
  return decryptWithConversationKey(fromBase64(nameCiphertext), fromBase64(nameNonce), secret);
}

/** The owner's own copy of a link: unseals its secret with their key pair. */
export function openOwnedLink(link: CallLinkDto, publicKey: Uint8Array, privateKey: Uint8Array): OpenedCallLink | null {
  try {
    const secret = sodium.crypto_box_seal_open(fromBase64(link.secretSealed), publicKey, privateKey);
    return {
      id: link.id,
      kind: link.kind,
      name: openLinkName(link.nameCiphertext, link.nameNonce, secret),
      secret,
      url: callLinkUrl(link.id, secret),
      createdOn: link.createdOn,
    };
  } catch {
    return null;
  }
}

export function sealSecretForOwner(secret: Uint8Array, ownerPublicKey: Uint8Array): string {
  return toBase64(sodium.crypto_box_seal(secret, ownerPublicKey));
}

/** Offer/answer for a link call, sealed (XChaCha20-Poly1305) under the link's secret. */
export function sealLinkSignal(signal: CallSignal, secret: Uint8Array): SealedSignalDto {
  const { ciphertext, nonce } = encryptWithConversationKey(JSON.stringify(signal), secret);
  return { ciphertext: toBase64(ciphertext), nonce: toBase64(nonce) };
}

const MAX_SIGNAL_AGE_MS = 5 * 60 * 1000;

export function openLinkSignal(
  sealed: SealedSignalDto,
  secret: Uint8Array,
  expected: { type: CallSignal["type"]; callId: string; linkId: string; from: string; to: string },
  now = Date.now()
): CallSignal {
  let signal: CallSignal;
  try {
    signal = JSON.parse(decryptWithConversationKey(fromBase64(sealed.ciphertext), fromBase64(sealed.nonce), secret)) as CallSignal;
  } catch {
    throw new CallSignalError("This request couldn't be verified with the link's secret.");
  }
  const ok =
    signal.v === 1 &&
    signal.type === expected.type &&
    signal.callId === expected.callId &&
    signal.conversationId === `link:${expected.linkId}` &&
    signal.from === expected.from &&
    signal.to === expected.to &&
    (signal.kind === "voice" || signal.kind === "video") &&
    typeof signal.sdp === "string" &&
    /^a=fingerprint:/m.test(signal.sdp);
  if (!ok) throw new CallSignalError("Call setup didn't match this call.");
  if (typeof signal.sentAt !== "number" || Math.abs(now - signal.sentAt) > MAX_SIGNAL_AGE_MS) throw new CallSignalError("Call setup is too old.");
  return signal;
}

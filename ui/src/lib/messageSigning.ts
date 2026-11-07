// Message signatures (db/migrations/030, docs/group-keys-and-signatures.md).
//
// Everyone in a chat holds its key, so encryption alone can't tell members
// apart: any of them — or the server, given a member's key — could make a
// message look like someone else's. Each account's apps therefore sign every
// message with the account's Ed25519 key (lib/e2eCrypto.ts
// deriveSigningKeyPair), and readers check the signature against the
// sender's key, which is pinned on first sight like their encryption key.
import * as api from "./api.js";
import { ApiError } from "./api.js";
import * as crypto from "./e2eCrypto.js";

interface OwnSigner {
  userId: string;
  pair: crypto.SigningKeyPair;
  /** False until the server accepted the public key (until then signed messages would be refused). */
  registered: boolean;
}

let own: OwnSigner | null = null;
let registering: Promise<void> | null = null;

/** Called on sign-in: derives this account's signing key and registers its public half (set once on the server). */
export function beginMessageSigning(identity: { userId: string; privateKey: Uint8Array }): Promise<void> {
  if (own?.userId !== identity.userId) {
    own = { userId: identity.userId, pair: crypto.deriveSigningKeyPair(identity.privateKey), registered: false };
    registering = null;
  }
  return ensureRegistered();
}

export function stopMessageSigning(): void {
  own = null;
  registering = null;
  senderKeys.clear();
}

function ensureRegistered(): Promise<void> {
  const signer = own;
  if (!signer || signer.registered) return Promise.resolve();
  if (!registering) {
    registering = Promise.resolve()
      .then(() => api.registerSigningKey(crypto.toBase64(signer.pair.publicKey)))
      .then(() => {
        signer.registered = true;
        // Our own key is known now: don't keep answering "no key" for our own signed messages.
        senderKeys.set(idOf(signer.userId), { key: pin(signer.userId, signer.userId, signer.pair.publicKey), since: null });
      })
      .catch((err) => {
        // signing_key_exists: the server holds a different key for this account
        // (it never should); messages go out unsigned and readers flag them.
        if (!(err instanceof ApiError && err.code === "signing_key_exists")) registering = null; // Try again on the next send.
        console.warn("Message signing isn't available", err instanceof ApiError ? err.code : err);
      });
  }
  return registering;
}

/** This account's signature over a message about to be sent, or undefined when signing isn't set up (yet). */
export async function signatureFor(
  conversationId: string,
  keyEpoch: number,
  nonce: Uint8Array,
  ciphertext: Uint8Array
): Promise<string | undefined> {
  await ensureRegistered();
  if (!own?.registered) return undefined;
  try {
    const input = crypto.messageSignatureInput(conversationId, own.userId, keyEpoch, nonce, ciphertext);
    return crypto.toBase64(crypto.signMessage(input, own.pair.secretKey));
  } catch (err) {
    console.warn("Couldn't sign a message", err instanceof Error ? err.message : err);
    return undefined;
  }
}

// --- Checking other people's signatures ---

interface SenderKey {
  key: Uint8Array | null;
  /** "YYYY-MM-DD" the key was registered: group messages from then on should be signed. */
  since: string | null;
}

const senderKeys = new Map<string, SenderKey>();
const PIN_PREFIX = "redvaultsystem:signing-key:";

/** Ids are compared lowercased everywhere: another spelling of one id must never earn a second pin. */
const idOf = (userId: string) => userId.toLowerCase();

function pin(selfUserId: string, userId: string, key: Uint8Array): Uint8Array {
  const storageKey = `${PIN_PREFIX}${idOf(selfUserId)}:${idOf(userId)}`;
  try {
    const known = localStorage.getItem(storageKey);
    if (known) return crypto.fromBase64(known);
    localStorage.setItem(storageKey, crypto.toBase64(key));
  } catch {
    // Storage unavailable: trust this session's first answer only.
  }
  return key;
}

function storedPin(selfUserId: string, userId: string): Uint8Array | null {
  try {
    const known = localStorage.getItem(`${PIN_PREFIX}${idOf(selfUserId)}:${idOf(userId)}`);
    return known ? crypto.fromBase64(known) : null;
  } catch {
    return null;
  }
}

/** For a pinned signer the server says nothing about: they sign, so any date counts as after they started. */
const PINNED_SINCE = "0000-00-00";

/**
 * Makes sure the signing keys of these senders are known. The first key seen
 * for someone is pinned: a later, different answer from the server is
 * ignored, so their signatures with that key fail rather than a swapped key
 * being trusted silently.
 */
export async function resolveSenderKeys(selfUserId: string, userIds: Iterable<string>): Promise<void> {
  for (const userId of new Set([...userIds].map(idOf))) {
    if (senderKeys.has(userId)) continue;
    // Someone who signs doesn't stop: a pinned key holds, whatever the server answers now.
    const stored = own && idOf(own.userId) === userId ? own.pair.publicKey : storedPin(selfUserId, userId);
    try {
      const user = await api.getUser(userId);
      const key = stored ?? (user.signingPublicKey ? pin(selfUserId, userId, crypto.fromBase64(user.signingPublicKey)) : null);
      senderKeys.set(userId, { key, since: user.signingKeySince ?? (stored ? PINNED_SINCE : null) });
    } catch {
      // Unknown for now; checked again on the next load.
      if (stored) senderKeys.set(userId, { key: stored, since: PINNED_SINCE });
    }
  }
}

/**
 * These people just sent signed messages. Anyone we hold no key for is looked up again by the next
 * resolveSenderKeys: they registered a key after we last asked. Without this, a "no key yet" answer
 * stayed cached for the whole page session and their signed messages read as forged.
 */
export function forgetKeylessSenders(userIds: Iterable<string>): void {
  for (const userId of userIds) {
    const id = idOf(userId);
    if (senderKeys.has(id) && !senderKeys.get(id)!.key) senderKeys.delete(id);
  }
}

/**
 * - verified: a valid signature by the sender's pinned key.
 * - legacy: unsigned, from before the sender had a signing key.
 * - unverified: an unsigned message from someone whose apps sign, sent
 *   after they started: an older app of theirs — or a forgery. Shown with a
 *   warning; it can't edit, react, vote or pin.
 * - invalid: a signature that doesn't check out. Not shown.
 */
export type SenderCheck = "verified" | "legacy" | "unverified" | "invalid";

export function checkSender(
  message: { conversationId: string; senderId: string; keyEpoch?: number; nonce: string; ciphertext: string; signature?: string | null; createdAt: string },
  isGroup: boolean
): SenderCheck {
  const known = senderKeys.get(idOf(message.senderId));
  if (message.signature) {
    if (!known?.key) return "invalid";
    try {
      const input = crypto.messageSignatureInput(
        message.conversationId,
        message.senderId,
        message.keyEpoch ?? 0,
        crypto.fromBase64(message.nonce),
        crypto.fromBase64(message.ciphertext)
      );
      return crypto.verifyMessageSignature(crypto.fromBase64(message.signature), input, known.key) ? "verified" : "invalid";
    } catch {
      return "invalid";
    }
  }
  // Direct chats too: once someone signs, an unsigned message from them is a stripped signature or an old app.
  void isGroup;
  if (!known?.key || !known.since) return "legacy";
  return message.createdAt.slice(0, 10) >= known.since ? "unverified" : "legacy";
}

/** The pinned signing key of someone already looked up with resolveSenderKeys, or null. */
export function senderSigningKey(userId: string): Uint8Array | null {
  return senderKeys.get(idOf(userId))?.key ?? null;
}

export function canSignAsAccount(): boolean {
  return Boolean(own?.registered);
}

/** Signs with this account's key, for something other than a message. Null until the key is registered. */
export function signAsAccount(input: Uint8Array): Uint8Array | null {
  return own?.registered ? crypto.signMessage(input, own.pair.secretKey) : null;
}

/** Test helper. */
export function resetMessageSigning(): void {
  stopMessageSigning();
}

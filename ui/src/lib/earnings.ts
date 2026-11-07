// Creator earnings on this device: paying (and waiting for the server to
// confirm it), exclusive posts (encrypting, opening, and the creator app's
// job of sealing each buyer the key). The rules live on the server
// (docs/creator-earnings.md); the crypto in lib/postCrypto.ts.
import { ApiError, getUser } from "./api.js";
import * as crypto from "./e2eCrypto.js";
import * as eapi from "./earningsApi.js";
import { decryptFile, encryptFile } from "./fileCrypto.js";
import { newPostId, newPostKey, openPostBody, sealPostBody, unwrapPostKey, wrapPostKey, type PostBody } from "./postCrypto.js";

export interface EarningsIdentity {
  userId: string;
  publicKey: Uint8Array;
  privateKey: Uint8Array;
}

export class PaymentNotCompleted extends Error {
  constructor(readonly status: eapi.PurchaseStatus | "timeout") {
    super(
      status === "timeout"
        ? "We haven't heard from the payment provider yet. If you paid, it will show up shortly."
        : status === "refunded" || status === "refund_pending"
          ? "This payment was refunded."
          : "The payment didn't go through."
    );
  }
}

const POLL_MS = 2000;
const GIVE_UP_MS = 35 * 60 * 1000;

/**
 * Waits for the SERVER to confirm a purchase: it only marks one paid after
 * the provider's verified webhook, so coming back from the payment page
 * proves nothing on its own.
 */
export async function waitForPurchase(
  purchaseId: string,
  options: { signal?: AbortSignal; pollMs?: number; giveUpMs?: number; onStatus?: (s: eapi.PurchaseStatus) => void } = {}
): Promise<eapi.PurchaseDto> {
  const started = Date.now();
  for (;;) {
    if (options.signal?.aborted) throw new DOMException("Aborted", "AbortError");
    const p = await eapi.getPurchase(purchaseId);
    options.onStatus?.(p.status);
    if (p.status === "paid" || p.status === "consumed") return p;
    if (p.status !== "checkout_pending" && p.status !== "consuming") throw new PaymentNotCompleted(p.status);
    if (Date.now() - started > (options.giveUpMs ?? GIVE_UP_MS)) throw new PaymentNotCompleted("timeout");
    await new Promise((resolve) => setTimeout(resolve, options.pollMs ?? POLL_MS));
  }
}

/**
 * Starts a purchase and opens the provider's payment page in a new tab.
 * Returns the purchase id to wait on, or a purchase that's already usable
 * (an unused paid message: never charged twice).
 */
export async function startCheckout(input: Omit<eapi.StartPurchase, "idempotencyKey" | "platform">, idempotencyKey = eapi.newIdempotencyKey()) {
  const started = await eapi.startPurchase({ ...input, idempotencyKey, platform: "web" });
  if (started.checkoutUrl) {
    const opened = window.open(started.checkoutUrl, "_blank", "noopener,noreferrer");
    // Pop-ups blocked: go there in this tab; the purchase is picked up when the app comes back.
    if (!opened) window.location.assign(started.checkoutUrl);
  }
  return started;
}

// --- Exclusive posts -----------------------------------------------------------------------

export interface NewPost {
  text: string;
  priceMinor: number;
  teaser: string;
  media?: { blob: Blob; mime: string };
}

export async function publishPost(identity: EarningsIdentity, post: NewPost, onStage?: (stage: "encrypting" | "uploading") => void): Promise<string> {
  await crypto.ready();
  const id = newPostId();
  const key = newPostKey();
  onStage?.("encrypting");
  const body: PostBody = { v: 1, text: post.text.trim() };
  let encrypted: Awaited<ReturnType<typeof encryptFile>> | null = null;
  if (post.media) {
    encrypted = await encryptFile(post.media.blob);
    body.media = {
      key: crypto.toBase64(encrypted.key),
      header: crypto.toBase64(encrypted.header),
      sha256: encrypted.ciphertextSha256,
      encryptedSize: encrypted.encryptedSize,
      chunkSize: encrypted.chunkSize,
      mime: post.media.mime,
    };
  }
  const sealed = sealPostBody(id, key, body);
  onStage?.("uploading");
  await eapi.createPost({
    id,
    priceMinor: post.priceMinor,
    teaser: post.teaser.trim(),
    bodyCiphertext: crypto.toBase64(sealed.ciphertext),
    bodyNonce: crypto.toBase64(sealed.nonce),
    wrappedKey: crypto.toBase64(wrapPostKey(id, key, identity.publicKey, identity.privateKey)),
    mediaSize: encrypted ? encrypted.encryptedSize : null,
  });
  if (encrypted) await eapi.uploadPostMedia(id, encrypted.blob);
  return id;
}

/** The author's public key: from your chat with them when you have one (the key you've seen), else the server's. */
async function authorKey(authorId: string, known: Map<string, Uint8Array>): Promise<Uint8Array> {
  const mine = known.get(authorId);
  if (mine) return mine;
  return crypto.fromBase64((await getUser(authorId)).publicKey);
}

export interface OpenedPost {
  body: PostBody;
  key: Uint8Array;
}

/** Decrypts a post this account can read (its own, or one bought whose key has arrived). */
export async function openPost(identity: EarningsIdentity, post: eapi.PostDto, knownKeys: Map<string, Uint8Array> = new Map()): Promise<OpenedPost> {
  if (!post.body) throw new Error(post.awaitingKey ? "The creator's app hasn't sent your key yet." : "This post is locked.");
  await crypto.ready();
  const author = post.mine ? identity.publicKey : await authorKey(post.authorId, knownKeys);
  const key = unwrapPostKey(post.id, crypto.fromBase64(post.body.wrappedKey), author, identity.privateKey);
  return { body: openPostBody(post.id, key, crypto.fromBase64(post.body.ciphertext), crypto.fromBase64(post.body.nonce)), key };
}

export async function postMediaUrl(postId: string, body: PostBody): Promise<string | null> {
  if (!body.media) return null;
  const m = body.media;
  const blob = await decryptFile(
    await eapi.downloadPostMedia(postId),
    { key: crypto.fromBase64(m.key), header: crypto.fromBase64(m.header), chunkSize: m.chunkSize, encryptedSize: m.encryptedSize, ciphertextSha256: m.sha256 },
    m.mime
  );
  return URL.createObjectURL(blob);
}

/**
 * The creator's app: buyers whose payment the server confirmed but who
 * don't have the post's key yet get it sealed to their public key. Only
 * this app can do it (the server never holds the key); a buyer whose key
 * doesn't arrive within 72 hours is refunded automatically. A buyer you
 * chat with only gets it sealed to the key your chat shows (`knownKeys`), so
 * a server can't slip in a key of its own.
 */
export async function deliverPendingKeys(identity: EarningsIdentity, knownKeys: Map<string, Uint8Array> = new Map()): Promise<number> {
  await crypto.ready();
  const requests = await eapi.getKeyRequests();
  const keys = new Map<string, Uint8Array>();
  let delivered = 0;
  for (const r of requests) {
    try {
      const buyerKey = crypto.fromBase64(r.buyerPublicKey);
      const known = knownKeys.get(r.buyerId);
      if (known && crypto.toBase64(known) !== crypto.toBase64(buyerKey)) continue;
      let key = keys.get(r.postId);
      if (!key) {
        const post = await eapi.getPost(r.postId);
        if (!post.mine || !post.body) continue;
        key = unwrapPostKey(post.id, crypto.fromBase64(post.body.wrappedKey), identity.publicKey, identity.privateKey);
        keys.set(r.postId, key);
      }
      const sealed = wrapPostKey(r.postId, key, buyerKey, identity.privateKey);
      await eapi.deliverPostKey(r.postId, r.purchaseId, crypto.toBase64(sealed));
      delivered++;
    } catch (err) {
      // One that fails (refunded meanwhile, say) doesn't stop the rest.
      if (err instanceof ApiError && err.status === 401) throw err;
    }
  }
  return delivered;
}

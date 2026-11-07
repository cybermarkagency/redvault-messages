// Share links to files in someone's encrypted Files (…?file=<id>#<secret>).
//
// The desktop app makes these (desktop/core/src/drive_crypto.cpp and
// DriveRepository::createLink); this is the other end. The server keeps only
// ciphertext: the file's own key, its name and type are sealed with a key
// only the link holds. That key is the 32-byte secret in the URL fragment
// (never sent to any server), or, when the link has a password,
// BLAKE2b(secret, key = Argon2id(password, salt)), so the link alone isn't
// enough to open it.
import sodium from "libsodium-wrappers-sumo";
import { API_BASE } from "./apiBase.js";
import { secretFromFragment } from "./callLinkCrypto.js";
import { decryptFile, FileDecryptError } from "./fileCrypto.js";
import { decryptWithConversationKey, fromBase64, ready } from "./e2eCrypto.js";

export interface SharedFileLinkInfo {
  id: string;
  sealedMeta: string;
  metaNonce: string;
  passwordSalt: string | null;
  allowDownload: boolean;
  expiresAt: string | null;
  encryptedSize: number;
  ciphertextSha256: string;
}

export interface SharedFileMeta {
  name: string;
  mime: string;
  size: number;
  key: Uint8Array;
  header: Uint8Array;
  chunk: number;
  sha: string;
}

export class SharedFileLinkError extends Error {
  constructor(message: string, readonly code: string) {
    super(message);
    this.name = "SharedFileLinkError";
  }
}

export { secretFromFragment };

function linkUrl(linkId: string, suffix = ""): string {
  return `${API_BASE}/public/backup-links/${encodeURIComponent(linkId)}${suffix}`;
}

async function failure(res: Response): Promise<SharedFileLinkError> {
  let code = "";
  try {
    code = ((await res.json()) as { error?: string }).error ?? "";
  } catch {
    // Not JSON: fall through to the status.
  }
  if (res.status === 404) return new SharedFileLinkError("This link doesn't exist, or its owner removed it.", "not_found");
  if (res.status === 410 || code === "link_expired") return new SharedFileLinkError("This link has expired. Ask its owner for a new one.", "expired");
  if (res.status === 429) return new SharedFileLinkError("Too many tries. Wait a minute and try again.", "rate_limited");
  if (res.status === 503) return new SharedFileLinkError("Files aren't available on this server right now.", "unavailable");
  return new SharedFileLinkError("Couldn't open the link. Try again.", code || "failed");
}

export async function fetchSharedFileLink(linkId: string): Promise<SharedFileLinkInfo> {
  let res: Response;
  try {
    res = await fetch(linkUrl(linkId), { credentials: "omit", referrerPolicy: "no-referrer", cache: "no-store" });
  } catch {
    throw new SharedFileLinkError("Couldn't reach the server. Check your connection and try again.", "network");
  }
  if (!res.ok) throw await failure(res);
  return (await res.json()) as SharedFileLinkInfo;
}

/** The key that opens the link's metadata: the secret itself, or mixed with the password. */
export async function sharedLinkKey(secret: Uint8Array, password: string, saltB64: string | null): Promise<Uint8Array> {
  await ready();
  if (!saltB64) return secret;
  const salt = fromBase64(saltB64);
  if (salt.length !== sodium.crypto_pwhash_SALTBYTES) throw new SharedFileLinkError("This link is damaged.", "bad_link");
  const stretched = sodium.crypto_pwhash(
    32,
    password,
    salt,
    sodium.crypto_pwhash_OPSLIMIT_INTERACTIVE,
    sodium.crypto_pwhash_MEMLIMIT_INTERACTIVE,
    sodium.crypto_pwhash_ALG_ARGON2ID13
  );
  const key = sodium.crypto_generichash(32, secret, stretched);
  sodium.memzero(stretched);
  return key;
}

/** Opens the sealed metadata. Null when the key is wrong (a wrong password, or a damaged link). */
export function openSharedFileMeta(info: SharedFileLinkInfo, key: Uint8Array): SharedFileMeta | null {
  let json: string;
  try {
    // Padded with spaces to a 128-byte bucket before sealing.
    json = decryptWithConversationKey(fromBase64(info.sealedMeta), fromBase64(info.metaNonce), key).replace(/ +$/, "");
  } catch {
    return null;
  }
  try {
    const meta = JSON.parse(json) as { v?: number; name?: unknown; mime?: unknown; size?: unknown; key?: unknown; header?: unknown; chunk?: unknown; sha?: unknown };
    if (meta.v !== 1 || typeof meta.name !== "string" || typeof meta.key !== "string" || typeof meta.header !== "string" || typeof meta.sha !== "string") {
      return null;
    }
    return {
      name: meta.name,
      mime: typeof meta.mime === "string" && meta.mime ? meta.mime : "application/octet-stream",
      size: typeof meta.size === "number" ? meta.size : 0,
      key: fromBase64(meta.key),
      header: fromBase64(meta.header),
      chunk: typeof meta.chunk === "number" ? meta.chunk : 65536,
      sha: meta.sha,
    };
  } catch {
    return null;
  }
}

async function* readBody(body: ReadableStream<Uint8Array>): AsyncGenerator<Uint8Array> {
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

/** Downloads the ciphertext through the server and decrypts it here; checks size and hash before returning anything. */
export async function downloadSharedFile(
  info: SharedFileLinkInfo,
  meta: SharedFileMeta,
  onProgress?: (fraction: number) => void
): Promise<Blob> {
  // The server's hash and the sealed one must agree: the server can't swap the file.
  if (meta.sha !== info.ciphertextSha256) throw new FileDecryptError();
  let res: Response;
  try {
    res = await fetch(linkUrl(info.id, "/content"), { credentials: "omit", referrerPolicy: "no-referrer", cache: "no-store" });
  } catch {
    throw new SharedFileLinkError("Couldn't reach the server. Check your connection and try again.", "network");
  }
  if (!res.ok || !res.body) throw await failure(res);
  return decryptFile(
    readBody(res.body),
    { key: meta.key, header: meta.header, chunkSize: meta.chunk, encryptedSize: info.encryptedSize, ciphertextSha256: meta.sha },
    meta.mime,
    onProgress
  );
}

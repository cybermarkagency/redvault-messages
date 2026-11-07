// Sending a message body with whichever protocol the conversation allows:
// forward-secret (protocol 2) whenever this browser has a device and
// everyone in the conversation has at least one, otherwise the
// conversation key (protocol 1), which is what apps without protocol 2
// support can read.
//
// No silent downgrade: once a conversation has used protocol 2 on this
// browser (sent or received), it is pinned, and a send that could only go
// out with the conversation key is refused instead (DowngradeRefusedError).
//
// Protocol 1 messages carry their group key epoch and the sender's
// signature (lib/groupKeys.ts, lib/messageSigning.ts). When the server says
// the group needs a new key, or that the key changed, this rotates or
// re-reads the keys and sends again.
import * as api from "../api.js";
import { ApiError } from "../api.js";
import * as crypto from "../e2eCrypto.js";
import { currentKey, keyRingOwnerId, refreshKeyRing, rotateGroupKey } from "../groupKeys.js";
import { signatureFor } from "../messageSigning.js";
import { currentForwardSecretDevice, resetForwardSecretDevice } from "./device.js";
import { isPinnedForwardSecret, pinForwardSecret } from "./pins.js";
import { encryptForCommunity } from "../communityCrypto.js";
import { communityRingFor, loadCommunityRing, rotateCommunityKey } from "../communityKeys.js";

export interface SentMessage {
  dto: api.MessageDto;
  ciphertext: Uint8Array;
  nonce: Uint8Array;
}

/** Wraps the network call, e.g. to retry transient failures with the same request. */
type Retry = <T>(fn: () => Promise<T>) => Promise<T>;

const MAX_DEVICE_LIST_REFRESHES = 3;
const MAX_KEY_REFRESHES = 3;

/** Errors after which the message goes out with the conversation key instead — unless the conversation is pinned. */
const FALL_BACK_TO_CONVERSATION_KEY = new Set(["participant_without_devices", "e2e_v2_disabled", "device_not_registered"]);

/** A send refused because it would have gone out without forward secrecy in a chat that has it. */
export class DowngradeRefusedError extends Error {
  constructor() {
    super(
      "Not sent: this chat uses forward-secret encryption, and right now this message could only go out with the older, weaker protocol."
    );
  }
}

export async function sendEncryptedMessage(
  conversationId: string,
  conversationKey: Uint8Array,
  body: string,
  options: { attachments?: string[]; retry?: Retry; clientMessageId?: string; paid?: api.PaidSend } = {}
): Promise<SentMessage> {
  const retry: Retry = options.retry ?? ((fn) => fn());
  await crypto.ready();

  const ring = communityRingFor(conversationId);
  if (ring) return sendToCommunity(conversationId, body, options.attachments, retry, options.clientMessageId);

  const device = await currentForwardSecretDevice();
  const pinned = isPinnedForwardSecret(device?.userId ?? keyRingOwnerId(), conversationId);
  if (device) {
    let fallBack = false;
    for (let attempt = 0; attempt < MAX_DEVICE_LIST_REFRESHES && !fallBack; attempt++) {
      const prepared = await device.prepare(conversationId, body);
      if (!prepared) {
        fallBack = true;
        break;
      }
      try {
        const dto = await retry(() =>
          api.sendMessage(
            conversationId,
            crypto.toBase64(prepared.ciphertext),
            crypto.toBase64(prepared.nonce),
            options.attachments,
            prepared.envelope,
            undefined,
            options.clientMessageId,
            options.paid
          )
        );
        await device.rememberSent(dto.id, body);
        pinForwardSecret(device.userId, conversationId, dto.createdAt);
        return { dto, ciphertext: prepared.ciphertext, nonce: prepared.nonce };
      } catch (err) {
        if (!(err instanceof ApiError)) throw err;
        if (err.code === "device_not_registered") resetForwardSecretDevice();
        if (FALL_BACK_TO_CONVERSATION_KEY.has(err.code)) fallBack = true;
        else if (err.code !== "device_list_stale") throw err;
      }
    }
    // Never quietly downgraded just because the device list kept changing.
    if (!fallBack) throw new Error("Couldn't send: this conversation's devices keep changing. Try again.");
  }
  if (pinned) throw new DowngradeRefusedError();

  for (let attempt = 0; ; attempt++) {
    const { key, epoch } = currentKey(conversationId, conversationKey);
    const { ciphertext, nonce } = crypto.encryptWithConversationKey(body, key);
    const signature = await signatureFor(conversationId, epoch, nonce, ciphertext);
    try {
      const dto = await retry(() =>
        api.sendMessage(
          conversationId,
          crypto.toBase64(ciphertext),
          crypto.toBase64(nonce),
          options.attachments,
          undefined,
          { keyEpoch: epoch, signature },
          options.clientMessageId,
          options.paid
        )
      );
      if (options.clientMessageId && dto.ciphertext && dto.nonce && dto.ciphertext !== crypto.toBase64(ciphertext)) {
        // A retry of a send whose answer was lost: the server returned the copy it stored the first
        // time. Only this account knows its clientMessageId; check it's this message anyway.
        const stored = crypto.fromBase64(dto.ciphertext);
        const storedNonce = crypto.fromBase64(dto.nonce);
        let same: boolean | null = null;
        try {
          same = crypto.decryptWithConversationKey(stored, storedNonce, key) === body;
        } catch {
          same = null; // another key epoch: it can't be checked here
        }
        if (same === false) throw new Error("The server returned a different message.");
        return { dto, ciphertext: stored, nonce: storedNonce };
      }
      return { dto, ciphertext, nonce };
    } catch (err) {
      if (!(err instanceof ApiError) || attempt + 1 >= MAX_KEY_REFRESHES) throw err;
      // Someone left: give the group a new key first. The key changed: pick it up.
      if (err.code === "group_rekey_required") await rotateGroupKey(conversationId);
      else if (err.code === "stale_key_epoch") await refreshKeyRing(conversationId);
      else throw err;
    }
  }
}

/**
 * Communities: always the group key of the current epoch (never per-device
 * sessions). If someone left since this browser loaded the keys, the server
 * answers rekey_required: this app rotates the key (sealing a new one only to
 * who remains) and sends under it; stale_key_epoch means another app already
 * did, so it reloads the keys and tries once more.
 */
async function sendToCommunity(conversationId: string, body: string, attachments: string[] | undefined, retry: Retry, clientMessageId?: string): Promise<SentMessage> {
  for (let attempt = 0; ; attempt++) {
    const ring = communityRingFor(conversationId)!;
    const { ciphertext, nonce, keyEpoch } = encryptForCommunity(ring, body);
    try {
      const dto = await retry(() => api.sendMessage(conversationId, crypto.toBase64(ciphertext), crypto.toBase64(nonce), attachments, undefined, { keyEpoch }, clientMessageId));
      return { dto, ciphertext, nonce };
    } catch (err) {
      if (attempt >= 2 || !(err instanceof ApiError)) throw err;
      if (err.code === "rekey_required") await rotateCommunityKey(ring.communityId, conversationId).catch(() => loadCommunityRing(ring.communityId, conversationId));
      else if (err.code === "stale_key_epoch") await loadCommunityRing(ring.communityId, conversationId);
      else throw err;
    }
  }
}

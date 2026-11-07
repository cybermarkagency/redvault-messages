// The protocol-3 chat session of the signed-in account in this browser: its
// device, its history, and the mailbox poll (one at a time). Everything is
// off, and costs nothing, unless the server has E2E_V3_ENABLED, this build has
// the engine, and this browser holds a device that is in the account's signed
// list (docs/multi-device-threat-model.md section 9).
import * as api from "../api.js";
import { ChatHistory, chatAvailable, receiveChat, type ChatMessage } from "./chat.js";
import { V3Device } from "./device.js";
import { deviceStorageAvailable } from "./deviceStore.js";
import { engineIncluded } from "./engine.js";

export interface ChatSession {
  device: V3Device;
  history: ChatHistory;
}

const RETRY_AFTER_NO_SESSION_MS = 30_000;

let current: { userId: string; promise: Promise<ChatSession | null>; at: number } | null = null;
let polling: Promise<ChatMessage[]> | null = null;
const listeners = new Set<(messages: ChatMessage[]) => void>();

async function build(userId: string): Promise<ChatSession | null> {
  if (!engineIncluded() || !deviceStorageAvailable()) return null;
  if (!(await chatAvailable())) return null;
  const device = await V3Device.open(userId);
  if (!device) return null;
  // A device that is only registered, waiting for approval, can't send or be sent to.
  const list = (await api.getV3DeviceList(userId)).deviceList;
  if (!list?.devices.some((d) => d.deviceId === device.deviceId)) return null;
  return { device, history: new ChatHistory(device) };
}

/** The session for `userId`, or null when protocol 3 isn't usable here (checked again every 30 s). */
export function chatSession(userId: string): Promise<ChatSession | null> {
  if (current && current.userId === userId) {
    return current.promise.then((session) => {
      if (session || Date.now() - current!.at < RETRY_AFTER_NO_SESSION_MS) return session;
      current = null;
      return chatSession(userId);
    });
  }
  const promise = build(userId).catch(() => null);
  current = { userId, promise, at: Date.now() };
  return promise;
}

/** Sign-out or a different account: forget the session (the device itself stays in this browser's store). */
export function closeChatSession(): void {
  current = null;
  polling = null;
}

/** Subscribes to messages that arrive through the mailbox. */
export function onChatMessages(listener: (messages: ChatMessage[]) => void): () => void {
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}

/** Tells listeners about messages that did not come through the mailbox (this device's own sends). */
export function announceChat(messages: ChatMessage[]): void {
  for (const listener of [...listeners]) listener(messages);
}

/** The newest protocol-3 message of each conversation, for the chat list ({} when protocol 3 isn't usable here). */
export async function latestChats(userId: string): Promise<ChatMessage[]> {
  const session = await chatSession(userId);
  return session ? session.history.latestPerConversation() : [];
}

/** Checks the mailbox now (a second call while one is running shares its result). */
export function pollChat(userId: string): Promise<ChatMessage[]> {
  if (!polling) {
    polling = (async () => {
      const session = await chatSession(userId);
      if (!session) return [];
      const fresh = await receiveChat(session.device, session.history);
      if (fresh.length) for (const listener of [...listeners]) listener(fresh);
      return fresh;
    })()
      .catch(() => [] as ChatMessage[])
      .finally(() => {
        polling = null;
      });
  }
  return polling;
}

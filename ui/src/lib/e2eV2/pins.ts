// Downgrade protection for forward-secret messaging (protocol 2).
//
// Once a conversation has used protocol 2 on this browser — a message sent
// with it, or one received and decrypted — it is pinned here: from then on
// this browser refuses to send in it with the conversation key (protocol 1),
// and marks protocol-1 messages that arrive after the pin as sent without
// forward secrecy, so a server (or an outdated app) can't quietly take the
// chat back to the weaker protocol.
//
// Pins are per browser and only hold conversation ids and a timestamp, so
// they live in localStorage. allowLegacyProtocol is the explicit way back
// (for example if forward-secret messaging is switched off server-side).
const PREFIX = "redvaultsystem:fs-pinned:";

type Pins = Record<string, string>;

function read(userId: string): Pins {
  try {
    const raw = localStorage.getItem(PREFIX + userId);
    return raw ? (JSON.parse(raw) as Pins) : {};
  } catch {
    return {};
  }
}

function write(userId: string, pins: Pins): void {
  try {
    localStorage.setItem(PREFIX + userId, JSON.stringify(pins));
  } catch {
    // Storage unavailable: the pin lasts for this page only (see memory below).
  }
}

/** Pins that couldn't be stored still hold for the rest of this page's life. */
const memory = new Map<string, Pins>();

function all(userId: string): Pins {
  return { ...read(userId), ...(memory.get(userId) ?? {}) };
}

/** Pins the conversation from `at` (ISO time of the first forward-secret message), keeping an earlier pin. */
export function pinForwardSecret(userId: string, conversationId: string, at: string): void {
  const pins = all(userId);
  const existing = pins[conversationId];
  if (existing && existing <= at) return;
  pins[conversationId] = at;
  memory.set(userId, pins);
  write(userId, pins);
}

export function isPinnedForwardSecret(userId: string | null, conversationId: string): boolean {
  return userId !== null && all(userId)[conversationId] !== undefined;
}

/** When the conversation was pinned, or null. */
export function forwardSecretSince(userId: string, conversationId: string): string | null {
  return all(userId)[conversationId] ?? null;
}

/** The user's explicit choice to let this chat use the older protocol again. */
export function allowLegacyProtocol(userId: string, conversationId: string): void {
  const pins = all(userId);
  delete pins[conversationId];
  memory.set(userId, pins);
  write(userId, pins);
}

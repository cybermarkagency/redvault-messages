// Group call links: the address of a call that's going on in a group chat.
// The member who starts the call posts it in the chat, and the others paste
// it into "Join with a link" to join.
//
// There's no secret in it. The server only lets members of that group in
// (anyone else is told the call has ended), and the call's media and keys
// are end-to-end encrypted on their own (lib/groupCallCrypto.ts).
import type { CallKind } from "./api.js";
import { API_BASE } from "./apiBase.js";
import { pastedUrl } from "./pastedUrl.js";

const PARAM = "group-call";
const CALL_ID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

/** The shareable address: this app's, with the call's id. */
export function groupCallLinkUrl(callId: string): string {
  const base = new URL(API_BASE.replace(/\/api\/?$/, "/") || "/", window.location.origin);
  base.search = `?${PARAM}=${encodeURIComponent(callId)}`;
  return base.toString();
}

const UUID = CALL_ID;
const KINDS: readonly CallKind[] = ["voice", "video"];

/**
 * An address that starts a call in a group: another app (the desktop app) can
 * hand a call over to the web app with it. The web app starts the call as the
 * signed-in user, so the plan check and the chat link work as usual.
 */
export function groupCallStartUrl(conversationId: string, kind: CallKind): string {
  const base = new URL(API_BASE.replace(/\/api\/?$/, "/") || "/", window.location.origin);
  base.search = `?start-group-call=${encodeURIComponent(conversationId)}&kind=${kind}`;
  return base.toString();
}

/** The group and kind in a "start a call" address, or null. */
export function parseGroupCallStart(text: string): { conversationId: string; kind: CallKind } | null {
  const url = pastedUrl(text);
  const conversationId = url?.searchParams.get("start-group-call") ?? "";
  const kind = url?.searchParams.get("kind") as CallKind | null;
  return UUID.test(conversationId) && kind && KINDS.includes(kind) ? { conversationId: conversationId.toLowerCase(), kind } : null;
}

/** What the starter's app posts in the chat. */
export function groupCallInviteText(kind: CallKind, url: string): string {
  return `📞 Group ${kind} call started. To join, open Calls, tap "Join with a link" and paste this link:\n${url}`;
}

/** The call id in a pasted group call link (or in the chat message that carries it), or null. */
export function parseGroupCallLink(text: string): string | null {
  const callId = pastedUrl(text)?.searchParams.get(PARAM) ?? "";
  return CALL_ID.test(callId) ? callId.toLowerCase() : null;
}

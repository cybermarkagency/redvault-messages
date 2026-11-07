import { useCallback, useEffect, useRef, useState } from "react";
import type { CallKind } from "../lib/api.js";
import { parseGroupCallLink, parseGroupCallStart } from "../lib/groupCallLink.js";
import { useAppState } from "../state/AppStateContext.js";
import { useGroupCall } from "../state/GroupCallContext.js";

const PENDING = "redvaultsystem:pending-group-call";
/** How long a link waits for sign-in-and-load to finish before telling the user it couldn't be opened. */
const GIVE_UP_MS = 15_000;

export type GroupCallDeepLink = { join: string } | { start: string; kind: CallKind };

/** …/?group-call=<call id> joins that call; …/?start-group-call=<group id>&kind=video starts one. */
export function parseGroupCallDeepLink(href: string): GroupCallDeepLink | null {
  const callId = parseGroupCallLink(href);
  if (callId) return { join: callId };
  const start = parseGroupCallStart(href);
  return start ? { start: start.conversationId, kind: start.kind } : null;
}

function readPending(): GroupCallDeepLink | null {
  try {
    const raw = sessionStorage.getItem(PENDING);
    return raw ? (JSON.parse(raw) as GroupCallDeepLink) : null;
  } catch {
    return null;
  }
}

function clearPending(): void {
  try {
    sessionStorage.removeItem(PENDING);
  } catch {
    // Nothing stored to clear.
  }
}

/**
 * Opening the app from a group call address (e.g. handed over from the desktop
 * app) joins or starts that call once signed in. The address is taken out of
 * the address bar and history right away and kept only for this tab; a link
 * that can't be opened is reported in `notice`.
 */
export function useGroupCallDeepLink(): { notice: string | null; dismiss: () => void } {
  const { state } = useAppState();
  const groupCalls = useGroupCall();
  const [notice, setNotice] = useState<string | null>(null);
  const [version, setVersion] = useState(0);
  const inMemory = useRef<GroupCallDeepLink | null>(null);

  useEffect(() => {
    const link = parseGroupCallDeepLink(window.location.href);
    if (!link) return;
    inMemory.current = link;
    try {
      sessionStorage.setItem(PENDING, JSON.stringify(link));
    } catch {
      // Without session storage the link is only kept until the page reloads.
    }
    const params = new URLSearchParams(window.location.search);
    for (const name of ["group-call", "start-group-call", "kind"]) params.delete(name);
    const rest = params.toString();
    window.history.replaceState(null, "", `${window.location.pathname}${rest ? `?${rest}` : ""}`);
    setVersion((v) => v + 1);
  }, []);

  const pending = () => inMemory.current ?? readPending();
  const consume = () => {
    inMemory.current = null;
    clearPending();
  };

  const signedIn = state.authStatus === "signed-in" && !state.locked;
  const { available, joinByLink, start } = groupCalls;

  useEffect(() => {
    if (!signedIn) return;
    const link = pending();
    if (!link || !available) return;
    if ("join" in link) {
      consume();
      joinByLink(link.join).catch((err: unknown) => setNotice(err instanceof Error ? err.message : "Couldn't join the call. Try again."));
      return;
    }
    const group = state.conversations.find((c) => c.kind === "group" && c.id === link.start);
    if (!group || group.kind !== "group") return;
    consume();
    start(group, link.kind);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [signedIn, available, state.conversations, joinByLink, start, version]);

  useEffect(() => {
    if (!signedIn || !pending()) return;
    const timer = setTimeout(() => {
      if (!pending()) return;
      consume();
      setNotice("Couldn't open that group call. Group calls may be unavailable right now, or the group isn't on this account.");
    }, GIVE_UP_MS);
    return () => clearTimeout(timer);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [signedIn, version]);

  const dismiss = useCallback(() => setNotice(null), []);
  return { notice, dismiss };
}

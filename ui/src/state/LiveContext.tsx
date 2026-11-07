// The live channel to the server (GET /api/live/events, a long poll) and
// what rides on it: call signaling (handed to CallProvider), typing
// indicators, and contacts' online status.
//
// Both typing and online status are opt-in and reciprocal: this app only
// sends "typing" while you have typing indicators on, and only shows
// others' while you do; the server only reports who's online while you
// share your own status (server/src/services/liveService.ts).
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import * as api from "../lib/api.js";
import { ApiError, type LiveEventDto } from "../lib/api.js";
import { readPrefs } from "../lib/localPrefs.js";
import { useAppState } from "./AppStateContext.js";

interface LiveValue {
  /** The server has a call relay and this browser can do WebRTC. */
  callsAvailable: boolean;
  /** The server has a LiveKit SFU for group calls and this browser can do WebRTC. */
  groupCallsAvailable: boolean;
  /** userIds of contacts online right now. */
  online: ReadonlySet<string>;
  /** Who's typing in this conversation right now. */
  typingIn(conversationId: string): string[];
  /** Call on each keystroke; throttled, and a no-op unless typing indicators are on. */
  notifyTyping(conversationId: string): void;
  subscribe(listener: (event: LiveEventDto) => void): () => void;
}

const LiveCtx = createContext<LiveValue | null>(null);

const TYPING_SHOWN_MS = 6000;
const TYPING_SEND_EVERY_MS = 3000;
const PRESENCE_POLL_MS = 20_000;

export function LiveProvider({ children }: { children: ReactNode }) {
  const { state, dispatch } = useAppState();
  const signedInUserId = state.authStatus === "signed-in" ? state.identity?.userId : undefined;
  const { typingIndicators, showOnlineStatus } = state.settings;
  const [callsEnabled, setCallsEnabled] = useState(false);
  const [groupCallsEnabled, setGroupCallsEnabled] = useState(false);
  const [online, setOnline] = useState<ReadonlySet<string>>(new Set());
  const [typing, setTyping] = useState<Map<string, Map<string, number>>>(new Map());
  const listeners = useRef(new Set<(event: LiveEventDto) => void>());
  const typingOnRef = useRef(typingIndicators);
  typingOnRef.current = typingIndicators;
  const lastSent = useRef(new Map<string, number>());

  const subscribe = useCallback((listener: (event: LiveEventDto) => void) => {
    listeners.current.add(listener);
    return () => {
      listeners.current.delete(listener);
    };
  }, []);

  // Typing indicators are remembered on this browser (like the phone and desktop apps), so a new sign-in starts from the last choice.
  useEffect(() => {
    if (signedInUserId) dispatch({ type: "UPDATE_SETTINGS", settings: { typingIndicators: readPrefs(signedInUserId).typingIndicators } });
  }, [signedInUserId, dispatch]);

  // The long poll: runs while signed in.
  useEffect(() => {
    if (!signedInUserId) return;
    const abort = new AbortController();
    let cursor: number | null = null;
    (async () => {
      let failures = 0;
      while (!abort.signal.aborted) {
        try {
          const res = await api.callEvents(cursor, abort.signal);
          if (!res || typeof res.cursor !== "number") throw new Error("bad_response");
          cursor = res.cursor;
          failures = 0;
          setCallsEnabled(res.callsEnabled !== false);
          setGroupCallsEnabled(res.groupCallsEnabled === true);
          for (const event of res.events) {
            if (event.type === "typing") {
              if (!typingOnRef.current) continue;
              setTyping((current) => {
                const next = new Map(current);
                const inChat = new Map(next.get(event.conversationId) ?? []);
                inChat.set(event.userId, Date.now() + TYPING_SHOWN_MS);
                next.set(event.conversationId, inChat);
                return next;
              });
              continue;
            }
            for (const listener of [...listeners.current]) await listener(event);
          }
        } catch (err) {
          if (abort.signal.aborted) return;
          // Calls off on an older server, no live channel at all, or signed out.
          if (err instanceof ApiError && (err.code === "calls_disabled" || err.status === 404 || err.status === 401)) {
            setCallsEnabled(false);
            return;
          }
          failures += 1;
          await new Promise((r) => setTimeout(r, Math.min(30_000, 2000 * failures)));
        }
      }
    })();
    return () => {
      abort.abort();
      setCallsEnabled(false);
    };
  }, [signedInUserId]);

  // Typing dots fade out on their own.
  useEffect(() => {
    if (typing.size === 0) return;
    const timer = setInterval(() => {
      const now = Date.now();
      setTyping((current) => {
        let changed = false;
        const next = new Map<string, Map<string, number>>();
        for (const [conversationId, users] of current) {
          const live = new Map([...users].filter(([, until]) => until > now));
          if (live.size !== users.size) changed = true;
          if (live.size > 0) next.set(conversationId, live);
        }
        return changed ? next : current;
      });
    }, 1000);
    return () => clearInterval(timer);
  }, [typing.size]);

  // Online status, while you share yours.
  useEffect(() => {
    if (!signedInUserId || !showOnlineStatus) {
      setOnline(new Set());
      return;
    }
    let cancelled = false;
    const refresh = () => {
      Promise.resolve()
        .then(() => api.getPresence())
        .then((res) => {
          if (!cancelled && res) setOnline(new Set(res.online));
        })
        .catch(() => {});
    };
    refresh();
    const timer = setInterval(refresh, PRESENCE_POLL_MS);
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  }, [signedInUserId, showOnlineStatus]);

  const typingIn = useCallback(
    (conversationId: string) => {
      const now = Date.now();
      return [...(typing.get(conversationId) ?? [])].filter(([, until]) => until > now).map(([userId]) => userId);
    },
    [typing]
  );

  const notifyTyping = useCallback((conversationId: string) => {
    if (!typingOnRef.current) return;
    const now = Date.now();
    if (now - (lastSent.current.get(conversationId) ?? 0) < TYPING_SEND_EVERY_MS) return;
    lastSent.current.set(conversationId, now);
    Promise.resolve()
      .then(() => api.sendTyping(conversationId))
      .catch(() => {});
  }, []);

  const callsAvailable = callsEnabled && typeof RTCPeerConnection !== "undefined";
  const groupCallsAvailable = groupCallsEnabled && typeof RTCPeerConnection !== "undefined";
  const value = useMemo(
    () => ({ callsAvailable, groupCallsAvailable, online, typingIn, notifyTyping, subscribe }),
    [callsAvailable, groupCallsAvailable, online, typingIn, notifyTyping, subscribe]
  );
  return <LiveCtx.Provider value={value}>{children}</LiveCtx.Provider>;
}

export function useLive(): LiveValue {
  const ctx = useContext(LiveCtx);
  if (!ctx) throw new Error("useLive must be used within a LiveProvider");
  return ctx;
}

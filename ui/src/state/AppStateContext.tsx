import * as api from "../lib/api.js";
import { beginForwardSecrecy, stopForwardSecrecy } from "../lib/e2eV2/device.js";
import { setKeyRingOwner } from "../lib/groupKeys.js";
import { beginMessageSigning, stopMessageSigning } from "../lib/messageSigning.js";
import { createContext, useContext, useEffect, useMemo, useReducer, useRef, type Dispatch, type ReactNode } from "react";
import { runCommunityKeyMaintenance } from "../lib/communityKeys.js";
import type { DMConversation } from "../types.js";
import { beginOutbox, drainOutbox, endOutbox, loadConversations, loadFriendRequests } from "./actions.js";
import { type Action, type AppState, createInitialState, reducer } from "./reducer.js";

interface AppStateValue {
  state: AppState;
  dispatch: Dispatch<Action>;
  /** DM conversations keyed by id — the contact list, for member pickers etc. */
  contactsById: Map<string, DMConversation>;
}

const AppStateCtx = createContext<AppStateValue | null>(null);

const CONVERSATIONS_POLL_MS = 5000;
const OUTBOX_DRAIN_MS = 15_000;
/** How often this app checks for community members waiting for a key, or a key that must rotate. */
const COMMUNITY_KEYS_POLL_MS = 20_000;
const PLAN_POLL_MS = 60_000;

export function AppStateProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(reducer, undefined, createInitialState);

  // This browser's forward-secret device (lib/e2eV2) and message signing: started in the
  // background on sign-in, forgotten on sign-out. Declared before the
  // polling below so the first load already uses it.
  useEffect(() => {
    if (state.authStatus !== "signed-in" || !state.identity) return;
    beginForwardSecrecy(state.identity);
    // Group key epochs and message signatures (docs/group-keys-and-signatures.md).
    setKeyRingOwner(state.identity);
    beginMessageSigning(state.identity).catch(() => {});
    return () => {
      stopForwardSecrecy();
      stopMessageSigning();
      setKeyRingOwner(null);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state.authStatus, state.identity?.userId]);

  // Loads once right after sign-in, then keeps polling — there's no
  // push/websocket transport, so this is also how a *newly received*
  // conversation (someone else adding you as a contact) ever shows up.
  // The reducer preserves locally-set state (verified, unread, …) across
  // repeated loads, so polling here is safe.
  useEffect(() => {
    if (state.authStatus !== "signed-in" || !state.identity) return;
    const identity = state.identity;
    const tick = () => {
      loadConversations(dispatch, identity).catch((err) => console.error("Failed to load conversations", err));
      loadFriendRequests(dispatch, identity).catch((err) => console.error("Failed to load friend requests", err));
    };
    tick();
    const interval = setInterval(tick, CONVERSATIONS_POLL_MS);
    return () => clearInterval(interval);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state.authStatus, state.identity?.userId]);

  // The outbox (lib/outbox.ts): opened on sign-in, sent every 15 s and as soon as the browser says
  // it's back online. Its key goes on sign-out.
  const conversationsRef = useRef(state.conversations);
  conversationsRef.current = state.conversations;
  useEffect(() => {
    if (state.authStatus !== "signed-in" || !state.identity) return;
    const identity = state.identity;
    let stopped = false;
    const drain = () => {
      if (!stopped) void drainOutbox(dispatch, conversationsRef.current);
    };
    beginOutbox(dispatch, identity).then(drain, () => {});
    const interval = setInterval(drain, OUTBOX_DRAIN_MS);
    window.addEventListener("online", drain);
    return () => {
      stopped = true;
      clearInterval(interval);
      window.removeEventListener("online", drain);
      endOutbox();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state.authStatus, state.identity?.userId]);

  // Communities: deliver keys to members who are waiting, rotate after someone
  // left (lib/communityKeys.ts). Runs while signed in; idle when there's nothing to do.
  useEffect(() => {
    if (state.authStatus !== "signed-in" || !state.identity) return;
    const tick = () => {
      runCommunityKeyMaintenance()
        .then(({ rotated, enrolled }) => {
          if (rotated + enrolled > 0 && state.identity) return loadConversations(dispatch, state.identity);
        })
        .catch(() => {});
    };
    const first = setTimeout(tick, 3_000);
    const interval = setInterval(tick, COMMUNITY_KEYS_POLL_MS);
    return () => {
      clearTimeout(first);
      clearInterval(interval);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state.authStatus, state.identity?.userId]);

  // The plan can change while signed in (a Stripe payment, a cancellation, an
  // admin): re-read it every minute and whenever the tab comes back into view.
  useEffect(() => {
    if (state.authStatus !== "signed-in" || !state.identity) return;
    const current = state.identity.subscriptionTier;
    const refresh = () => {
      Promise.resolve()
        .then(() => api.me())
        .then((me) => {
          if (me && me.subscriptionTier !== current) dispatch({ type: "SUBSCRIPTION_TIER_SET", tier: me.subscriptionTier });
        })
        .catch(() => {});
    };
    const interval = setInterval(refresh, PLAN_POLL_MS);
    const onVisible = () => {
      if (!document.hidden) refresh();
    };
    document.addEventListener("visibilitychange", onVisible);
    return () => {
      clearInterval(interval);
      document.removeEventListener("visibilitychange", onVisible);
    };
  }, [state.authStatus, state.identity?.userId, state.identity?.subscriptionTier]);

  const contactsById = useMemo(() => {
    const map = new Map<string, DMConversation>();
    for (const c of state.conversations) {
      if (c.kind === "dm") map.set(c.id, c);
    }
    return map;
  }, [state.conversations]);

  const value = useMemo(() => ({ state, dispatch, contactsById }), [state, contactsById]);

  return <AppStateCtx.Provider value={value}>{children}</AppStateCtx.Provider>;
}

export function useAppState(): AppStateValue {
  const ctx = useContext(AppStateCtx);
  if (!ctx) throw new Error("useAppState must be used within an AppStateProvider");
  return ctx;
}

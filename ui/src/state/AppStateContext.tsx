// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { createContext, useContext, useEffect, useMemo, useReducer, type Dispatch, type ReactNode } from "react";
import * as api from "../lib/api.js";
import { checkSession } from "./actions.js";
import { type Action, type AppState, createInitialState, reducer, type Screen } from "./reducer.js";

interface AppStateValue {
  state: AppState;
  dispatch: Dispatch<Action>;
}

const AppStateCtx = createContext<AppStateValue | null>(null);

const PLAN_POLL_MS = 60_000;

export function AppStateProvider({ initialScreen, children }: { initialScreen?: Screen; children: ReactNode }) {
  const [state, dispatch] = useReducer(reducer, initialScreen, createInitialState);

  // Still signed in from earlier in this browser (the session cookie)? Then no sign-in is needed.
  useEffect(() => {
    void checkSession(dispatch);
  }, []);

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

  const value = useMemo(() => ({ state, dispatch }), [state]);
  return <AppStateCtx.Provider value={value}>{children}</AppStateCtx.Provider>;
}

export function useAppState(): AppStateValue {
  const ctx = useContext(AppStateCtx);
  if (!ctx) throw new Error("useAppState must be used within an AppStateProvider");
  return ctx;
}

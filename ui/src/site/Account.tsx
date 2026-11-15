// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { AdminPanel } from "../screens/AdminPanel.js";
import { CommunityPurchase, Memberships } from "../screens/CommunityMemberships.js";
import { PlanPicker } from "../screens/settings/PlanPicker.js";
import { useAppState } from "../state/AppStateContext.js";
import type { Screen } from "../state/reducer.js";
import styles from "./Site.module.css";

type AccountScreen = Extract<Screen, { key: "plans" | "memberships" | "community" | "admin" }>;

/** Signed in: the admin panel (site admins only), the plan, and paid community memberships. */
export function Account({ screen }: { screen: AccountScreen }) {
  const { state, dispatch } = useAppState();
  const isAdmin = state.identity?.role === "super_admin";
  const tabs: { key: "admin" | "plans" | "memberships"; label: string }[] = [
    ...(isAdmin ? [{ key: "admin" as const, label: "Admin panel" }] : []),
    { key: "plans", label: "Plan" },
    { key: "memberships", label: "Community memberships" },
  ];
  const active = screen.key === "community" ? null : screen.key;

  return (
    <>
      <nav className={styles.tabs} aria-label="Account">
        {tabs.map((t) => (
          <button
            key={t.key}
            type="button"
            className={`${styles.tab} ${active === t.key ? styles.tabActive : ""}`}
            aria-current={active === t.key ? "page" : undefined}
            onClick={() => dispatch({ type: "NAVIGATE", screen: { key: t.key } })}
          >
            {t.label}
          </button>
        ))}
      </nav>
      {screen.key === "admin" && isAdmin && (
        <div className={styles.wide}>
          <AdminPanel />
        </div>
      )}
      {screen.key === "plans" && (
        <div className={styles.page}>
          <h1 className={`${styles.title} type-display`}>Your plan</h1>
          <p className={styles.text}>Your plan applies to your account in every RedVault Messages app. A change shows up in the apps within a minute.</p>
          <PlanPicker />
        </div>
      )}
      {screen.key === "memberships" && (
        <div className={styles.page}>
          <h1 className={`${styles.title} type-display`}>Community memberships</h1>
          <Memberships checkoutId={screen.checkoutId} canceled={screen.canceled} />
        </div>
      )}
      {screen.key === "community" && (
        <div className={styles.page}>
          <CommunityPurchase
            communityId={screen.communityId}
            inviteToken={screen.inviteToken}
            onMemberships={() => dispatch({ type: "NAVIGATE", screen: { key: "memberships" } })}
          />
        </div>
      )}
    </>
  );
}

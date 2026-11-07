import { useEffect, useRef } from "react";
import { useAppState } from "../state/AppStateContext.js";

const PENDING = "redvaultsystem:pending-community-link";

export type CommunityDeepLink =
  | { kind: "community"; communityId: string; inviteToken?: string }
  | { kind: "checkout"; subscriptionId: string }
  | { kind: "onboarding" }
  | { kind: "memberships" };

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

/**
 * …/?community=<id>&invite=<token> opens a community (an invite link);
 * …/?community-checkout=<subscription id>&result=… is Stripe sending a member
 * back (the payment is confirmed by asking the server, never by this address);
 * …/?creator-onboarding=… is Stripe's onboarding return; ?community-memberships
 * is the billing portal's return.
 */
export function parseCommunityDeepLink(href: string): CommunityDeepLink | null {
  let url: URL;
  try {
    url = new URL(href);
  } catch {
    return null;
  }
  const p = url.searchParams;
  const community = p.get("community");
  if (community && UUID.test(community)) {
    const invite = p.get("invite");
    return { kind: "community", communityId: community.toLowerCase(), ...(invite && /^[A-Za-z0-9_-]{10,100}$/.test(invite) ? { inviteToken: invite } : {}) };
  }
  const checkout = p.get("community-checkout");
  if (checkout && UUID.test(checkout)) return { kind: "checkout", subscriptionId: checkout.toLowerCase() };
  if (p.has("creator-onboarding")) return { kind: "onboarding" };
  if (p.has("community-memberships")) return { kind: "memberships" };
  return null;
}

/** Opens a community link once signed in, after taking it out of the address bar (invite tokens don't linger in history). */
export function useCommunityDeepLink(): void {
  const { state, dispatch } = useAppState();
  const pending = useRef<CommunityDeepLink | null>(null);

  useEffect(() => {
    const link = parseCommunityDeepLink(window.location.href);
    if (link) {
      pending.current = link;
      try {
        sessionStorage.setItem(PENDING, JSON.stringify(link));
      } catch {
        // Kept in memory only.
      }
      const url = new URL(window.location.href);
      for (const key of ["community", "invite", "community-checkout", "result", "creator-onboarding", "community-memberships"]) url.searchParams.delete(key);
      window.history.replaceState(window.history.state, "", url.toString());
    } else {
      try {
        const raw = sessionStorage.getItem(PENDING);
        if (raw) pending.current = JSON.parse(raw) as CommunityDeepLink;
      } catch {
        // Nothing stored.
      }
    }
  }, []);

  useEffect(() => {
    const link = pending.current;
    if (!link || state.authStatus !== "signed-in") return;
    pending.current = null;
    try {
      sessionStorage.removeItem(PENDING);
    } catch {
      // Nothing to clear.
    }
    if (link.kind === "community") dispatch({ type: "OPEN_COMMUNITIES", page: "community", communityId: link.communityId, inviteToken: link.inviteToken });
    else if (link.kind === "checkout") dispatch({ type: "OPEN_COMMUNITIES", page: "memberships", checkoutId: link.subscriptionId });
    else if (link.kind === "onboarding") dispatch({ type: "OPEN_COMMUNITIES", page: "creator" });
    else dispatch({ type: "OPEN_COMMUNITIES", page: "memberships" });
  }, [state.authStatus, dispatch]);
}

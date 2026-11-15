// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The addresses this website can be opened at, besides its own pages.
//
// The apps share links that point at the website (their WEB_APP_BASE):
// group invites (?invite=<id>#<secret>), call links (?join=<id>#<secret>),
// group calls (?group-call=<id>, ?start-group-call=<group>&kind=…), channels
// (?channel=<id>, #<secret> when private) and communities
// (?community=<id>&invite=<token>). Messaging lives only in the apps, so the
// website just says to open the link there. Its secret (the #fragment) is
// never read, kept or sent: the address bar is cleaned right away.
//
// Stripe sends people back here after a payment: ?community-checkout=<id>
// (a community membership), ?community-memberships (the billing portal) and
// ?creator-onboarding (a creator's payout setup). The address proves nothing;
// a membership is confirmed by asking the server.

export type AppLinkKind = "group-invite" | "call-link" | "group-call" | "channel" | "community" | "payment" | "story" | "profile";

export type SiteLink =
  /** A link meant for the apps. `communityId` (and `inviteToken`) when it is a community's: it can also be bought here. */
  | { kind: "app"; what: AppLinkKind; communityId?: string; inviteToken?: string }
  | { kind: "community-checkout"; subscriptionId: string; canceled: boolean }
  | { kind: "community-memberships" }
  | { kind: "creator-onboarding" };

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const INVITE_TOKEN = /^[A-Za-z0-9_-]{10,100}$/;

/** Query parameters that only mean something to the apps, in the order they're checked. */
const APP_PARAMS: [string, AppLinkKind][] = [
  ["group-call", "group-call"],
  ["start-group-call", "group-call"],
  ["join", "call-link"],
  ["channel", "channel"],
  ["invite", "group-invite"],
  ["purchase", "payment"],
  ["story", "story"],
  ["profile", "profile"],
];

/** Every parameter a link can carry; all of them leave the address bar. */
const LINK_PARAMS = [
  "community",
  "invite",
  "community-checkout",
  "result",
  "community-memberships",
  "creator-onboarding",
  "kind",
  ...APP_PARAMS.map(([param]) => param),
];

export function parseSiteLink(href: string): SiteLink | null {
  let url: URL;
  try {
    url = new URL(href);
  } catch {
    return null;
  }
  const p = url.searchParams;
  const community = p.get("community");
  if (community !== null) {
    const invite = p.get("invite");
    return UUID.test(community)
      ? { kind: "app", what: "community", communityId: community.toLowerCase(), ...(invite && INVITE_TOKEN.test(invite) ? { inviteToken: invite } : {}) }
      : { kind: "app", what: "community" };
  }
  const checkout = p.get("community-checkout");
  if (checkout && UUID.test(checkout)) return { kind: "community-checkout", subscriptionId: checkout.toLowerCase(), canceled: p.get("result") === "cancel" };
  if (p.has("community-memberships")) return { kind: "community-memberships" };
  if (p.has("creator-onboarding")) return { kind: "creator-onboarding" };
  for (const [param, what] of APP_PARAMS) if (p.has(param)) return { kind: "app", what };
  return null;
}

/**
 * Takes the link's parameters and the whole #fragment out of the address bar
 * and this tab's history, without reloading. Other parameters stay.
 */
export function cleanAddressBar(): void {
  const url = new URL(window.location.href);
  const hadLink = LINK_PARAMS.some((param) => url.searchParams.has(param));
  if (!hadLink && !url.hash) return;
  for (const param of LINK_PARAMS) url.searchParams.delete(param);
  const rest = url.searchParams.toString();
  window.history.replaceState(null, "", `${url.pathname}${rest ? `?${rest}` : ""}`);
}

/** How the "open it in the app" page names each kind of link. */
export const APP_LINK_NAMES: Record<AppLinkKind, string> = {
  "group-invite": "a group invite",
  "call-link": "a call link",
  "group-call": "a group call",
  channel: "a channel",
  community: "a community",
  payment: "a payment in a chat",
  story: "a story",
  profile: "a profile",
};

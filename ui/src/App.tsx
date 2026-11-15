// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The RedVault Messages website. Messaging happens only in the apps (Android,
// Windows, Linux); the website keeps the landing page with the downloads, the
// public transparency feed, plans and payments, and the admin panel. The
// public share-link page is its own bundle (share/, src/share/).
//
// Nothing here makes or registers device keys, fetches or decrypts messages,
// or opens the live connection: signing in only proves the password (as a
// sign-in key) and the authenticator code.

import { useEffect, useState } from "react";
import { TorGate } from "./components/TorGate.js";
import { cleanAddressBar, parseSiteLink } from "./lib/siteLinks.js";
import { TransparencyFeed } from "./screens/TransparencyFeed.js";
import { Account } from "./site/Account.js";
import { Landing } from "./site/Landing.js";
import { CreatorOnboardingReturn, OpenInApp } from "./site/OpenInApp.js";
import { SignIn } from "./site/SignIn.js";
import { SiteFrame } from "./site/SiteFrame.js";
import { AppStateProvider, useAppState } from "./state/AppStateContext.js";
import { homeFor, type Screen } from "./state/reducer.js";

interface Startup {
  screen: Screen;
  /** The address as opened, when it was a link meant for the apps (for Copy link; memory only). */
  originalLink: string | null;
}

/** What the address the page was opened at asks for. Reads it only: the effect in App cleans it. */
export function readStartup(href: string = window.location.href): Startup {
  const link = parseSiteLink(href);
  if (!link) return { screen: { key: "home" }, originalLink: null };
  switch (link.kind) {
    case "app":
      return { screen: { key: "app-link", what: link.what, communityId: link.communityId, inviteToken: link.inviteToken }, originalLink: href };
    case "community-checkout":
      return { screen: { key: "memberships", checkoutId: link.subscriptionId, canceled: link.canceled }, originalLink: null };
    case "community-memberships":
      return { screen: { key: "memberships" }, originalLink: null };
    case "creator-onboarding":
      return { screen: { key: "creator-onboarding" }, originalLink: null };
  }
}

function Site({ originalLink }: { originalLink: string | null }) {
  const { state, dispatch } = useAppState();
  const { screen } = state;
  const identity = state.authStatus === "signed-in" ? state.identity : null;

  let page;
  switch (screen.key) {
    case "home":
      page = identity ? <AccountFor screen={homeFor(identity)} /> : <Landing />;
      break;
    case "sign-in":
      page = <SignIn />;
      break;
    case "transparency":
      page = <TransparencyFeed onBack={() => dispatch({ type: "NAVIGATE", screen: { key: "home" } })} />;
      break;
    case "app-link":
      page = <OpenInApp what={screen.what} communityId={screen.communityId} inviteToken={screen.inviteToken} originalLink={originalLink} />;
      break;
    case "creator-onboarding":
      page = <CreatorOnboardingReturn />;
      break;
    default:
      page = identity ? <AccountFor screen={screen} /> : <SignIn />;
  }
  return <SiteFrame>{page}</SiteFrame>;
}

function AccountFor({ screen }: { screen: Screen }) {
  if (screen.key === "plans" || screen.key === "memberships" || screen.key === "community" || screen.key === "admin") return <Account screen={screen} />;
  return null;
}

export default function App() {
  const [startup] = useState(() => readStartup());
  // A link's secret (#fragment) and parameters leave the address bar and history right away.
  useEffect(() => cleanAddressBar(), []);

  return (
    // Tor routing: on the clearnet address, says where the site is moving (or, once enforced, runs only there).
    <TorGate>
      <AppStateProvider initialScreen={startup.screen}>
        <Site originalLink={startup.originalLink} />
      </AppStateProvider>
    </TorGate>
  );
}

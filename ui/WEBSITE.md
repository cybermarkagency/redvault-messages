# The RedVault Messages website (ui/)

Since 2026-11-08 messaging happens only in the apps (Android, Windows, Linux).
This package is the website that is left. It keeps four things:

| What | Where |
| --- | --- |
| Admin panel (users, reports, admin messages and polls, communities) | `src/screens/AdminPanel.tsx`, `AdminMessages.tsx`, `AdminCommunities.tsx` |
| Public share-link page (own bundle, own stricter CSP) | `share/index.html`, `src/share/` (unchanged) |
| Plans and payments: Pro/Premium (Stripe links, switch, cancel, keep), paid community memberships (buy from a community link, cancel, keep, card portal, receipts, Stripe return) | `src/screens/settings/PlanPicker.tsx`, `src/lib/planChoices.ts`, `src/screens/CommunityMemberships.tsx` |
| Public transparency feed | `src/screens/TransparencyFeed.tsx` |

Plus the file viewer for `?file=<id>#<secret>` links from the apps' Files
(`src/screens/SharedFile.tsx`, opened by `src/main.tsx`), the Terms and
Privacy pages (`terms.html`, `privacy.html`, built from `../legal/`), and the
AGPL source offer (`OpenSourceNotice`, `SourceCodeLink`, in the footer).

## Pages (`src/App.tsx`, `src/state/reducer.ts`)

- **Signed out:** landing page (`src/site/Landing.tsx`) with the three app
  downloads, Sign in, the transparency feed, Terms, Privacy and the source
  offer. No sign-up: accounts are made in the apps.
- **Sign in** (`src/site/SignIn.tsx`): prelogin + sign-in key
  (`src/lib/authKey.ts`, pins included), then the authenticator code. The
  session cookie is checked on load, so a reload (or Stripe's redirect back)
  stays signed in. The website never unwraps the account's private key.
- **Signed in:** site admins land on the admin panel; everyone has Plan and
  Community memberships tabs (`src/site/Account.tsx`), and Sign out.
- **Links the apps make** (`src/lib/siteLinks.ts`): `?invite=`, `?join=`,
  `?group-call=`, `?start-group-call=`, `?channel=`, `?community=`,
  `?purchase=` (and `?story=`/`?profile=` defensively) show "Open this in the
  RedVault Messages app" with the downloads. The parameters and the whole
  `#fragment` leave the address bar at once and are never sent anywhere; Copy
  link copies the original link to the clipboard only. A `?community=` link
  can also buy a paid membership here after signing in.
- **Stripe returns:** `?community-checkout=<id>&result=` opens Community
  memberships and asks the server to confirm; `?community-memberships` (the
  card portal) opens the same page; `?creator-onboarding` says to go back to
  the app.

## What the website never does

No device keys, no messages, no conversations, no live connection, no calls,
no Files, Stories, channels, vault or analytics. The crypto-core WebAssembly
engine isn't in any bundle; libsodium is (sign-in keys, the admin panel's
create-account form, and the file and share viewers). Without WebAssembly
(Tor Browser at Safer/Safest) the landing page still works; only signing in
and the file viewer explain why they can't. The CSP (`csp.ts`) and the
Tor gate (`src/components/TorGate.tsx`) are as before.

## Tests

`npx vitest run`: `src/test/site.test.tsx` covers the pages, links, sign-in,
two-step verification, plans, Stripe returns and that no messaging request
is ever made (against `src/test/fakeSiteServer.ts`). `shareEndToEnd.test.ts`
still runs the share page against the real server; the owner's side it needs
lives in `src/test/shareOwnerSide/` (test support only).

## Not done here

`package.json` still lists dependencies nothing imports any more
(`livekit-client`, `jsqr`, `qrcode`, `@noble/hashes`, `@noble/post-quantum`,
`@types/qrcode`, `fake-indexeddb`): removing them changes the lockfile, which
needs an `npm install` on a machine that doesn't share `node_modules`.

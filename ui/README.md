# ui — Redvaultsystem Messenger

A web implementation of the Redvaultsystem Messenger developer handoff (Sep 17, 2026):
design tokens, typography, screen state machine, and component specs
implemented as-specified. This is a **self-contained prototype** — local
mock state only, matching the exact data model from the handoff
(`DMConversation` / `GroupConversation` / `Message` / `AppSettings`), no
backend wiring. It's the reference for porting to Android/Windows/macOS,
the same role the handoff's own interactive build serves.

## Develop

```
npm install
npm run dev         # http://localhost:5173
npm test            # vitest: pure-logic + reducer + RTL flow tests
npm run typecheck
npm run build        # production build to dist/
```

## Licence and source offer

The web app is licensed under the GNU AGPL v3 only (`AGPL-3.0-only`, see
`LICENSE`). Under AGPL section 13, people using it over the network must be
offered its source, so Settings > About and the sign-in screens show an
"Open-Source Licenses and Source Code" section (`src/components/OpenSourceNotice.tsx`).
It is configured at build time (`src/lib/sourceOffer.ts`):

| Variable | Meaning |
| --- | --- |
| `VITE_SOURCE_CODE_URL` | https base URL of the published source for this version (e.g. a repository). Empty, unset or not https: the app says the link will appear once it is published, and shows no link. |
| `VITE_SOURCE_CONTACT` | Where to report problems getting the source: an email address or an https URL. Empty or unset: the app points to the contact address in the Terms of Service. |

```
VITE_SOURCE_CODE_URL=https://example.org/redvault VITE_SOURCE_CONTACT=source@example.org npm run build
```

The version shown is `version` from `package.json` (defined as
`__APP_VERSION__` in `vite.config.ts`). `public/legal/LICENSE.txt` and
`public/legal/THIRD_PARTY_NOTICES.txt` are copied into the build and served
at `<base>legal/` (e.g. `/redvaultmessages/legal/LICENSE.txt`).

## What's implemented

- **Design tokens** (`src/styles/tokens.css`): every light/dark color from
  the handoff's table, as CSS custom properties — nothing hardcoded
  elsewhere. Fraunces/IBM Plex Sans/IBM Plex Mono via `@fontsource`, spacing
  and radius tokens (`--radius-control` through `--radius-bubble-tail`).
- **Data model** (`src/types.ts`): the exact TS interfaces from the
  handoff. A group's verified count is computed live (`verifiedCount`),
  never stored.
- **Key glyph / safety number** (`src/lib/glyph.ts`): FNV-1a hash ->
  mulberry32 PRNG -> mirrored 5x5 grid / 60-digit fingerprint, exactly as
  specified. **Prototype note**: both are seeded from a display handle
  here; a production build must seed from the actual public key material
  instead (see the doc comments in `KeyGlyph.tsx` / `SafetyNumber.tsx`).
- **Screen state machine** (`src/state/reducer.ts`): every transition from
  the handoff's mermaid diagram, plus the back/cancel paths it implies
  (e.g. `new-contact` <- `new`). Verify's post-decision navigation branches
  on how it was entered (`origin: "new-contact" | "thread"`), matching the
  diagram's two distinct edges out of `verify`.
- **Lock overlay as a route guard**: locking doesn't just visually cover
  the app — `AppShell` renders *only* `LockOverlay` when locked, so
  conversation content is absent from the DOM/accessibility tree, not
  merely hidden under it. Backgrounding the tab triggers the same lock
  when both `appLock` and `switcherBlur` are on, as the closest web analog
  to the OS app-switcher trigger.
- **Responsive layout**: below 860px, phone-style single-pane navigation
  (`Sidebar` and the main pane are never both mounted); at/above 860px, the
  persistent sidebar + thread-by-default desktop layout the handoff
  describes explicitly.

## Not implemented (out of scope for this pass)

- Any actual cryptography, networking, or persistence — see
  `../client/src/crypto.ts` and `../server/` for that layer; wiring this UI
  to them is a follow-up, and would require reworking the server's
  username+password identity model to match Redvaultsystem's keypair-only,
  no-username design first.
- Native camera scanning (the "Scan" button synthesizes a fake code for the
  demo instead).
- The decorative phone/desktop device-frame chrome from the handoff
  (`--bezel`, frame radii) — that's presentation chrome for a marketing/demo
  shot of the app, not something a running app renders around itself.

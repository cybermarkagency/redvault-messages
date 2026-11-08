# redvaultmessage: Android app

A native Kotlin + Jetpack Compose client for the same backend as `ui/`.
It uses the same libsodium crypto as the web client (via lazysodium), so
Android and web users share accounts and can message each other.

**Status: feature-complete with the web app, plus extras.** Register, log in
and MFA; friends (add by handle, accept/deny/cancel); chats (send, receive,
delete, read receipts, disappearing timers, per-message security panel and
verification details, report; older messages page in from the top, day
headers, pictures shown in the chat with a full-size viewer, copy and select
text, search in a chat, select several messages to forward or delete, forward
to several chats, send several files at once, paste a picture, emoji button,
drafts kept per chat in memory, muted/typing/draft marks and a Favorites chip
in the chat list, and "Share to RedVault" from other apps); **end-to-end encrypted files** (same format
as the web, up to the server's limit); **safety numbers with QR scanning**
between two phones; the public transparency feed; profile photos; settings;
**group invite links** (Group info > Invite link, "Join a group with a link"
under New chat); **Channels** (the Channels tab: public or private with a
link, photos, view counts, forwarding into chats, formatted posts, photos /
videos / audio / files / voice clips / round videos, polls, quizzes,
checklists, scheduled and suggested posts, new-post notifications); and
**content-free push notifications** without Google services (below).

| | |
|---|---|
| Package name | `com.redvaultsystems.redvaultmessage` (permanent once on Play) |
| Min / target SDK | 26 (Android 8.0) / 37 |
| Backend | `redvaultApiBase` in `gradle.properties`: the service's onion address, through the built-in Tor gateway |

## Run it on your phone

1. Install [Android Studio](https://developer.android.com/studio). It bundles the JDK and Android SDK.
2. **File → Open**, then pick this `android/` folder (not the repo root). Let the Gradle sync finish.
   If Studio offers to install a missing SDK platform or upgrade the Gradle plugin, accept.
3. On the phone, go to **Settings → About phone** and tap **Build number** 7 times,
   then turn on **Developer options → USB debugging**.
4. Plug the phone in over USB, accept the "Allow USB debugging" prompt, pick it in the
   device dropdown, and press **Run ▶**.

No phone? Use **Device Manager** in Studio to create an emulator.

### Against a local dev server

Start the server on port 3000, then build with:

```
./gradlew installDebug -PredvaultApiBase=http://10.0.2.2:3000/api/
```

`10.0.2.2` is your computer as seen from the emulator. Plain `http` to it only works in
debug builds, and that debug build talks to it directly, without Tor.

### Tor by default

There is one app, and it always connects through Tor (`docs/tor-by-default.md`): the
default `redvaultApiBase` is the service's onion address, the Tor gateway (`tor-core`,
Arti) is built into every build, and everything but calls goes through it: the account,
messages on either protocol (device keys and Signal included), files, Stories, avatars,
the live channel and the background wake connection. Calls use WebRTC through the TURN
relay (relay-only); their signalling still goes through the API, so through Tor. A
release build refuses any other API base (`checkTorOnlyRelease` in
`app/build.gradle.kts`), and `network_security_config.xml` allows plain `http` only to
the onion host. Links people share (share, invite, call and channel links) point at the
public website (`redvaultWebAppBase`), which the app never connects to itself.

## Tests

- `./gradlew test`: JVM unit tests (key glyph matches the web client's). Crypto that
  needs libsodium runs here too, on lazysodium-java (the same library and version, with
  desktop builds of libsodium): `GroupInviteCryptoInteropTest` opens invite links the web
  client's own code made, and checks the keys they derive. **If it fails, don't ship.**
- `./gradlew connectedAndroidTest` (with a phone or emulator connected):
  `ConversationRepositoryTest` covers decryption, unread/read state, file
  descriptors and the send-side commitment check against a fake server, and
  `E2eCryptoCompatTest` checks every crypto primitive against vectors produced by
  the web client's own libsodium. **If this fails, don't ship.** Android and web
  users would be unable to log in to each other's accounts or read each other's messages.

## Notifications (no Firebase; through Tor, the background connection only)

Through Tor (every release build) the app uses no third-party push service at all:
notifications come only from the background connection below, through the onion
service. UnifiedPush is never registered, and one registered by an earlier version is
removed (`push/PushRegistrar.kt`). What follows about UnifiedPush applies only to a
debug build pointed at a local test server.

There's no Firebase or Google Play services in the app. Pushes are
content-free: the server only ever sends `{"kind": "message"}` (or
`friend_request`, `friend_accepted`, `call`, `share_upload`), never a
sender, chat or text, and the phone shows a generic "You have a new message".
They reach the phone one of two ways, picked after sign-in
(`push/PushRegistrar.kt`):

1. **UnifiedPush**, when the server has a VAPID key and a UnifiedPush
   distributor app is installed, such as [ntfy](https://ntfy.sh) (F-Droid or
   Play). After sign-in the app asks the distributor for an endpoint (Android
   shows a chooser if several are installed and none is the default; Settings
   has **Choose push app** later) and sends it to `PUT /api/push/webpush` with
   the Web Push keys. The server encrypts each push to this phone (Web Push,
   RFC 8291, signed with VAPID); the distributor's server only carries
   ciphertext and learns when a push arrives, and the connector in the app
   decrypts it (`push/UnifiedPushReceiver.kt`).
2. **Background connection**, when there's no distributor or the server can't
   send Web Push (`/api/push/config` gives no key): a foreground service
   (`push/PushFallbackService.kt`, type `specialUse`) long-polls
   `GET /api/push/wait` through the app's own API client, so Tor routing
   applies, with a quiet "Background connection" notification. It backs off
   on errors (up to about 5 minutes) and runs only while signed in: the
   session lives in memory, so after a reboot or a killed app it starts again
   at the next sign-in (there's no boot receiver). Settings has a
   **Background connection when no push app is installed** switch (on by
   default). Installing ntfy saves the battery this costs.

Server side, once: `npm run vapid:generate`, then set `VAPID_PRIVATE_KEY` and
`VAPID_SUBJECT` (and `WEBPUSH_ALLOWED_HOSTS` for the push servers you accept,
e.g. your own ntfy) in the server environment; see `docs/unifiedpush-plan.md`.
Without them every phone uses the background connection.

The app asks for notification permission after sign-in (Android 13+);
without it the background connection doesn't start. Settings shows how
notifications arrive ("Push through ntfy", "Background connection" or "Off").
The Notifications switch turns both off: nothing is registered and no
connection is kept. Strict no-push ("never a push service run by someone else")
is always on through Tor, with no switch: the background connection still runs
(its own switch), because it reaches only the onion service through the Tor
gateway, never directly (tests in `tor/TorRoutingTest.kt`, `privacy/NoMetadataTest.kt`). Logging out unregisters from the distributor, removes the
endpoint from the server and stops the background connection.

## Releasing to Google Play

### 1. Create your upload key (once, and keep it safe)

```
keytool -genkeypair -v -keystore redvault-upload.jks -keyalg RSA -keysize 4096 -validity 10000 -alias upload
```

(`keytool` ships with Android Studio's JDK: `<Studio>/jbr/bin/keytool`.) Put the
`.jks` file in this folder and create `android/keystore.properties`:

```
storeFile=redvault-upload.jks
storePassword=...
keyAlias=upload
keyPassword=...
```

Both are git-ignored. **Back up the keystore and passwords somewhere safe.**
With Play App Signing a lost upload key can be reset, but it takes time.

### 2. Build the bundle

Bump `versionCode` (and `versionName`) in `app/build.gradle.kts` for every upload, then:

```
./gradlew bundleRelease
```

The output is `app/build/outputs/bundle/release/app-release.aab`.

### 3. Play Console

1. Create a developer account at <https://play.google.com/console> ($25, one-time).
2. **Create app**, enable **Play App Signing**, and upload the `.aab`.
3. Personal developer accounts must run a **closed test with at least 12 testers for
   14 continuous days** before production access is granted. Organization accounts
   (which need a D-U-N-S number) are exempt.
4. Fill in the store listing, content rating, target audience, and a **privacy policy URL**.
5. **Data safety** form, as the app currently behaves:
   - Messages and files: end-to-end encrypted; the server can't read them.
   - Data collected: username (account management) and optional profile photo.
     No phone number, email, contacts, location or device identifiers.
   - Data shared with third parties: none. No ads, analytics or crash-reporting SDKs.
   - Data encrypted in transit: yes. Users can delete their data: answer to match
     what the server supports.

## Open-source licences and the source offer

The app is licensed AGPL-3.0-only (builds with `-PredvaultE2eV3=true -PredvaultE2eSignal=true`
also link libsignal, AGPL-3.0-only). **Settings > Open-Source Licenses and Source Code** shows the
licence notice, the version, the full licence (`app/src/main/assets/legal/LICENSE.txt`), the
third-party notices (`legal/THIRD_PARTY_NOTICES.txt`, generated by
`tools/compliance/third_party_notices.py`; don't edit it by hand) and the font and icon licences in
`app/src/main/assets/licenses/`. Two Gradle properties (in `gradle.properties` or with `-P`) fill
in the rest:

| Property | Meaning | Empty (default) |
|---|---|---|
| `redvaultSourceCodeUrl` | `https://` URL of the source for exactly this build, shown as "Source code for this version". `{version}` in it is replaced with `versionName`, e.g. `https://example.org/archive/v{version}.tar.gz`. Anything but an `https://` URL with a host fails the build at configuration time. | No link; the app says the link will appear once the source is published. |
| `redvaultSourceContact` | Where to report source-availability problems: an e-mail address (opens the mail app) or an `https://` page. One line. | The app points to the contact address in the Terms of Service (and opens them). |

```
./gradlew bundleRelease -PredvaultSourceCodeUrl=https://<host>/<path-for-this-version> -PredvaultSourceContact=<address>
```

Set `redvaultSourceCodeUrl` for every build given to anyone once the source is published, and
point it at that version's source, not just the newest. `OpenSourceInfoTest` checks the URL rules
and that every file the screen opens is bundled.

## Layout

```
app/src/main/java/com/redvaultsystems/redvaultmessage/
  crypto/     E2eCrypto (port of ui/src/lib/e2eCrypto.ts), Commitment, Base64Codec
  identity/   Glyph, HandleGenerator, LastHandleStore, SelfIdentity
  data/       AuthRepository, ConversationRepository, local stores; api/ holds the Retrofit
              service, DTOs and in-memory session cookie jar; model/ the chat models
  ui/         AuthViewModel, theme (from ui/src/styles/tokens.css), onboarding/, components/,
              chats/ (ChatSession: in-memory keys + refresh loops; chat list and thread screens)
```

## Security notes

- The unwrapped private key, conversation keys and decrypted messages, like the
  session cookie, live **only in memory**; conversation keys are overwritten on log out. When
  Android kills the app, the user logs in again (same as a web reload).
- Backups and device-to-device transfer are disabled for all app data.
- Release builds trust only system CAs and refuse plain HTTP.
- Requests carry no `User-Agent` (the server strips it anyway).
- Decrypted files live only in the app's private cache (`cache/decrypted/`)
  and are deleted on logout and on the next app start. Open hands a file to
  another app with a one-time read grant; Save writes it where the user picks.
- Direct-to-storage transfers use an HTTP client with no cookie jar, so the
  session cookie never reaches a third-party host.
- The app only refreshes while it's on screen; in the background it relies on
  the content-free pushes above (UnifiedPush or its own background connection).
- Safety-number verification is stored on the device only, keyed by the contact's
  public key, so a changed key shows as unverified again.
- A group invite link's secret is only ever in the link's `#fragment` and on the
  phone; the server gets ciphertext and a token derived from it.
- Channels are plain text by design (and labelled so). Reading them, their photos
  and their media goes out on a client with no cookie jar (`ApiClient.publicClient`),
  so the server can't tell which account reads or watches what; which channels you
  follow is kept only on the phone. Videos and recordings play through that same
  client by byte ranges (`ChannelMediaClient`), never the platform player's own HTTP
  stack and its User-Agent.
- A private channel's link secret is only ever in the link's `#fragment` and on the
  phone (the follow list, and for the owner sealed to their own key on the server);
  readers send a token derived from it (`X-Channel-Access`).
- View counts are anonymous: each phone reports a post once, by post id only.
  Poll votes, podcast positions and what was already notified stay on the phone.
- New-post notifications are worked out on the phone (it checks each followed channel
  every couple of minutes while the app runs), so the server still keeps no follower
  list. On the lock screen they only say "New post in a channel you follow", and with
  app lock on that's all they ever say.
- Photos posted to a channel keep their full resolution, but JPEG, PNG and WebP
  photos lose their location tags first. Videos are uploaded as recorded.

## Icons

The UI icons are [Lucide](https://lucide.dev) (ISC license), the same set as the
desktop app: the SVGs live in `desktop/app/icons/lucide/`, and
`python android/tools/lucide_to_vector.py` converts the ones listed in that script
into `res/drawable/ic_*.xml`. Don't edit those files by hand; add the icon to the
script and re-run it. The license ships in the APK as
`assets/licenses/lucide-LICENSE.txt`.

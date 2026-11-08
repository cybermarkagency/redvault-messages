# Redvault desktop client (C++)

A native desktop client that talks to the same `server/` API as the web
(`ui/`) and Android (`android/`) clients, with the same end-to-end
encryption.

## Layout

```
desktop/
  core/     Qt-free C++20 library: crypto, HTTP + cookie jar, API client/DTOs, auth repository
  cli/      redvault_cli: sign in to a real server and read your chats from the terminal
  tests/    Catch2 tests, including byte-for-byte compat vectors from the web client
```

`core/` ports `android/.../crypto/` one file at a time, using the same
libsodium primitives and parameters:

| Desktop (`redvault::crypto`)   | Android                 | Web                          |
|--------------------------------|-------------------------|------------------------------|
| `e2e_crypto.hpp`               | `E2eCrypto.kt`          | `ui/src/lib/e2eCrypto.ts`    |
| `commitment.hpp`               | `Commitment.kt`         | `ui/src/lib/commitment.ts`   |
| `file_crypto.hpp` (`files::`)  | `FileCrypto.kt`         | `ui/src/lib/fileCrypto.ts`   |
| `recovery_key.hpp`             | `RecoveryKey.kt`        | `ui/src/lib/recoveryKey.ts`  |
| `safety_number_qr.hpp`         | `SafetyNumberQr.kt`     | —                            |
| `base64.hpp`                   | `Base64Codec.kt`        | libsodium ORIGINAL variant   |
| `community_crypto.hpp` (`community::`) | —              | `ui/src/lib/communityCrypto.ts` |

The API side mirrors `android/.../data/`:

| Desktop                                   | Android                         |
|-------------------------------------------|---------------------------------|
| `api/api_client.hpp` (`ApiClient`)        | `ApiService.kt` + `apiCall()`   |
| `api/dtos.hpp`                            | `Dtos.kt`                       |
| `api/api_error.hpp`                       | `ApiException.kt`               |
| `net/cookie_jar.hpp`                      | `SessionCookieJar` (ApiClient.kt) |
| `data/auth_repository.hpp`                | `AuthRepository.kt`             |
| `data/conversation_repository.hpp`        | `ConversationRepository.kt`     |
| `data/friend_repository.hpp`              | `FriendRepository.kt`, `GroupRepository.kt` |
| `data/file_repository.hpp`                | `FileRepository.kt`             |
| `data/chat_models.hpp`                    | `model/ChatModels.kt`           |
| `model/rich_message.hpp`                  | `model/RichMessage.kt`          |
| `model/file_message.hpp`                  | `model/FileDescriptor.kt`       |
| `api/community_api.hpp`, `data/community_keys.hpp` | — (web: `communityApi.ts`, `communityKeys.ts`) |

Every `ApiClient` and repository call blocks; the UI runs them on a worker
thread. Tests use a `FakeTransport`, so nothing touches the network.

Key material (private keys, wrap keys, conversation keys) lives in
`SecretBytes`: `sodium_malloc` memory that is guard-paged, mlocked where the
OS allows, zeroed on free, and move-only.

## Building (Windows, MSYS2 UCRT64)

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,libsodium,catch,curl,nlohmann-json}
export PATH=/c/msys64/ucrt64/bin:$PATH

cd desktop
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Device keys with Signal sessions (`e2ev3/`, `REDVAULT_E2E_V3` and `REDVAULT_E2E_SIGNAL`, on by default and
in release builds) need Rust: install it with rustup (`~/.cargo/bin` on PATH), and CMake builds
`crypto-core` with cargo (`--locked`). `-DREDVAULT_E2E_V3=OFF` (or `-DREDVAULT_E2E_SIGNAL=OFF`) leaves it out
of a test build only. See `E2E_V3.md`.

With vcpkg/MSVC instead, install `libsodium`, `curl`, `nlohmann-json` and `catch2` and pass
`-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`; the
build picks up vcpkg's `unofficial-sodium` config automatically. On
Linux/macOS, install libsodium, libcurl, nlohmann-json and Catch2 v3 from the system package manager.

## The desktop app (Qt 6 / QML)

`app/` builds `redvaultmessages.exe` when Qt 6.5+ is installed (otherwise it's
skipped and the core, CLI and tests still build):

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{qt6-base,qt6-declarative,qt6-svg,qt6-tools,qtkeychain-qt6,qrencode}
# Calls: codecs, capture and echo cancellation (libdatachannel is fetched by CMake)
pacman -S --needed mingw-w64-ucrt-x86_64-{qt6-multimedia,opus,libvpx,libsrtp,openssl,webrtc-audio-processing,abseil-cpp}
# Group calls: protoc and the protobuf runtime for LiveKit's protocol
pacman -S --needed mingw-w64-ucrt-x86_64-protobuf
```

Calls (`rtc/`, `REDVAULT_BUILD_CALLS`, on by default) build libdatachannel
v0.24.6 from source through CMake's FetchContent, so the first configure needs
network access. `webrtc-audio-processing` is optional: without it calls work
but have no echo cancellation (use headphones). MSYS2's FFmpeg, which Qt
Multimedia uses, is a GPL build with many codecs, and the installer ships it.

Group calls (`groupcall/`, part of `REDVAULT_BUILD_CALLS`) run in the app through
LiveKit's prebuilt native client library, `livekit-ffi`. LiveKit's C++ SDK needs
MSVC and this app is built with MinGW, but the FFI library has a plain C interface,
so it doesn't matter what built it: CMake downloads the release's zip at a pinned
version and SHA-256 (`-DREDVAULT_LIVEKIT_FFI_ZIP=...` for offline builds), compiles
its protocol (`third_party/livekit-ffi/protocol`, vendored from the same release
tag) with `protoc`, and the app opens the library at run time from next to the
program (`livekit_ffi.dll`, or `lib/liblivekit_ffi.so` in the AppImage). Nothing links
against it. `tools/groupcall_probe` and `tools/groupcall_e2e` check the media path
and a whole desktop participant against a real LiveKit server and server without
the GUI; see `docs/group-calls.md`.

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH   # so Windows finds the Qt DLLs
.\build\debug\app\redvaultmessages.exe             # --api URL for another server
```

It's views, models and threading only: `AppController` (the QML `App`
singleton: sign-in, two-factor, sign-out) and `ChatSession` (the chat list
and open chat, polled every 5 s / 3 s like Android) run every core call on a
worker pool via `runInBackground` and deliver results on the UI thread. All
message content is shown with `PlainText`, so nothing from another person is
ever interpreted as markup. Only the last handle is saved (QSettings); keys
and the session cookie stay in memory and go when you sign out or quit.

### On the desktop

- **Tray and notifications.** Closing the window keeps a signed-in app
  running in the tray (Quit from the tray menu signs out). Notifications
  never contain message text, and by default not the sender either, since
  Windows keeps notification history; both are settings.
- **Hide from screenshots** (`SetWindowDisplayAffinity` with
  `WDA_EXCLUDEFROMCAPTURE`): screenshots, recordings and screen sharing see
  an empty window. Off by default, like Android's equivalent.
- **App lock.** A 4 to 12 digit PIN stored only as an Argon2id hash in the
  user's registry settings; locks after a chosen time in the background or
  with Ctrl+L; ten wrong PINs sign out. While locked the chats aren't even
  loaded.

### Icons

The app's icons are [Lucide](https://lucide.dev) v1.48.0 (ISC license), a
hand-picked set in `app/icons/lucide/` with its `LICENSE`, which the installer
ships as `licenses/lucide-LICENSE.txt`. The `icon` image provider recolors them
from the theme, so they follow light and dark mode. To add one, download it
from `https://cdn.jsdelivr.net/npm/lucide-static@1.48.0/icons/<name>.svg` into
that folder, list it under `RESOURCES` in `app/CMakeLists.txt`, and use
`Icon { name: "<name>" }` or `RvButton { iconName: "<name>" }`.

### Licence and source code

The app is free software under the GNU AGPL, version 3 only (AGPL-3.0-only); builds link
Signal's libsignal (AGPL-3.0-only) through the device-key engine (`REDVAULT_E2E_V3` and `REDVAULT_E2E_SIGNAL`,
both `ON` by default and in release builds; they need Rust/cargo). Settings > About has an
"Open-Source Licenses and Source Code" section (`app/qml/OpenSourceSection.qml`): the licence statement,
the version, this version's source link, the full licence and the third-party notices (read-only plain
text), where to report a problem getting the source, and that the app isn't affiliated with or endorsed by
Signal Messenger LLC. The two texts are `app/legal/LICENSE.txt` and `app/legal/THIRD_PARTY_NOTICES.txt`
(regenerate the notices with `tools/compliance/third_party_notices.py` for every release). They're built
into the app (`:/legal/`) and installed into `licenses/`, so they ship in the installer
(`dist\redvaultmessages\licenses`) and the AppImage (`usr/licenses`); both packaging scripts stop if either
is missing.

The source link and contact come from the build, never from the code:

| CMake variable | Meaning |
| --- | --- |
| `REDVAULT_SOURCE_CODE_URL` | `https://` URL of this version's source. `{version}` is replaced by the app version, e.g. `https://git.example.org/redvault/-/tree/v{version}`. Empty (the default): the section says the link will appear once the source is published, and shows no link. |
| `REDVAULT_SOURCE_CONTACT` | Email address or `https://` URL for reporting a problem getting the source. Empty (the default): the section points to the contact address in the Terms of Service. |

Anything other than an `https://` URL (or, for the contact, an email address) stops the configure step
(`cmake/source_info.cmake`), and the app checks the link again before showing it. `packaging\package.ps1`
and `packaging/package-linux.sh` pass both from environment variables of the same names, e.g.
`$env:REDVAULT_SOURCE_CODE_URL = "https://..."` before running the installer script.

## Tor by default

There is one app, and it sends everything except calls through the embedded Tor
gateway (`tor/`, tor-core with Arti) to Redvault's onion service
(`docs/tor-by-default.md`): the API, sign-in, messages on both protocols and
device keys/Signal, files, Files backups and share links, Stories, avatars, the
live connection and the update check. Calls send their media through the TURN
relays (relay-only, never Tor); their signalling goes through the API, so through
Tor. If Tor can't connect, sends wait in sealed queues on this computer and nothing
is retried any other way. Settings > Privacy & Security > Network routing shows it,
read-only.

| CMake variable | Default | Meaning |
| --- | --- | --- |
| `REDVAULT_TOR` | `ON` | Embed the Tor gateway (needs Rust/cargo). |
| `REDVAULT_TOR_ONLY` | `ON` | The onion service is the only server; any other address is refused at start. |
| `REDVAULT_ONION_API_BASE` | the official onion (`cmake/tor_settings.cmake`) | `http://<v3 onion>/redvaultmessages/api/` |
| `REDVAULT_RELEASE_BUILD` | `OFF` (`ON` in the release preset and every packaging script) | Refuses to configure unless the three above are as shipped. |

A developer may configure `-DREDVAULT_TOR_ONLY=OFF` for a local test build (to use
`REDVAULT_API_BASE=http://localhost:...`); CMake warns, and the release preset and
the packaging scripts refuse it (they also check `CMakeCache.txt` after configuring).
`tools/tor_routing_check` (with `REDVAULT_BUILD_TOOLS`) fetches
`<onion base>public/routing` through the app's own Tor transport and checks
`"viaOnion": true`.

## Installer

```powershell
powershell -ExecutionPolicy Bypass -File packaging\package.ps1
```

Release build, tests, then `cmake --install` into `dist\redvaultmessages` (the
exe, Qt via windeployqt, and every other DLL it needs from MSYS2), then
`dist\redvaultmessages-Setup-<version>.exe` via Inno Setup 6. It's a per-user
install (no admin): `%LOCALAPPDATA%\Programs\redvaultmessages` (upgrades keep
an existing install's folder), a Start menu entry and an uninstaller that also
removes this computer's app settings. The app is shown as "redvaultmessages";
internally it keeps the `Redvault` settings name, the installer AppId and the
update product id, so renaming it lost nobody's settings or upgrade path.
The installer isn't code-signed yet, so SmartScreen warns on first run.

### Microsoft Store (MSIX)

```powershell
powershell -ExecutionPolicy Bypass -File packaging\package-msix.ps1            # dist\redvaultmessages-<version>.msix
powershell -ExecutionPolicy Bypass -File packaging\package-msix.ps1 -Register  # try it on this PC (Developer Mode)
```

The same installed files plus `packaging/msix` (manifest template, logos) packed with
the Windows SDK's makeappx; the Store signs the package, so there's no SmartScreen
warning. Fill in `packaging/msix/store-identity.json` from Partner Center first. A
Store install has a package identity, which the app checks at startup
(`Updater::fromStore`): its own updater is then off, and Settings says the Store
updates it. Release steps: `HOW-TO-UPDATE-THE-APPS.txt`, section 9.

### Linux (AppImage)

```sh
sudo apt-get install ninja-build pkg-config libsodium-dev libcurl4-openssl-dev \
    nlohmann-json3-dev catch2 libqrencode-dev libgl1-mesa-dev libxkbcommon-dev \
    libopus-dev libvpx-dev libsrtp2-dev libssl-dev libwebrtc-audio-processing-dev \
    libprotobuf-dev protobuf-compiler
# and Rust (rustup) for the device-key engine
bash packaging/package-linux.sh     # needs Qt 6.5+ with Qt Multimedia (qmake on PATH, or QMAKE set)
```

Release build, tests, `cmake --install` into an AppDir, then linuxdeploy (downloaded
into `build/linuxdeploy`) bundles Qt and every library into
`dist/redvaultmessages-<version>-x86_64.AppImage`. The GitHub Actions workflow
`.github/workflows/desktop-linux.yml` does the same on Ubuntu 24.04 and uploads the
AppImage as a build artifact; it runs on glibc 2.39+ distributions. On Linux there
are no automatic updates (download the newest AppImage and replace the old one) and
no "Hide from screenshots" setting, since Linux has no way for a window to opt out
of capture. TLS uses the distribution's CA certificates.

## Automatic updates

Installed copies check
`https://www.redvaultsystems.com/redvaultmessages/desktop/latest.json` 15 s
after start and every 12 hours (a plain GET: no cookie, no identifying
headers), then offer "Update now". Nothing is trusted until it's verified
(`core/src/update/update.cpp`, tested in `tests/update_test.cpp`): the
manifest's Ed25519 signature against the public key built into the app
(`app/src/update_key.hpp`), a newer version only, an installer URL in that
`desktop/` folder only, and the installer's exact size and SHA-256. The
installer then runs silently and reopens the app (updating signs you out).
Talking to an onion service (every Tor-only build), the app fetches the same
files from that onion service instead, `http://<onion>/redvaultmessages/desktop/`,
through its Tor transport (`update::updateRoute`): the signed manifest still names
the published folder, and the download goes to the onion's copy of the same file,
checked by the same size and SHA-256. It never falls back to the normal internet;
while Tor isn't connected nothing is checked. If the onion service doesn't have the
files, the check finds nothing newer.

A manifest carries a signed `expires` date, 90 days out unless `sign` is given
`--valid-days N`. Past it the app warns that a newer version may exist (Settings,
the update banner, and once as a notification), because a signature can't show
that a manifest is the newest one. **Sign again before it runs out**, even with
no new release: same installer, same version. A manifest from before this field
is still accepted, with a warning in Settings until the next one is signed.

**The release key** is created once, on the release machine only:

```powershell
.\build\release\tools\release\redvault_release.exe keygen --embed app\src\update_key.hpp
```

It's saved to `%USERPROFILE%\.redvault\release-signing-key.json`, encrypted
with a passphrase you choose. Back up the file and the passphrase separately;
never put either on the server or in git. Losing them means installed copies
can't auto-update to new releases (people would reinstall by hand once).

**Releasing a version:**

1. Bump `project(RedvaultDesktop VERSION x.y.z ...)` in `CMakeLists.txt` (the
   only place the version lives).
2. Run `packaging\package.ps1`. With the key present it also asks for release
   notes and the key's passphrase, and writes `dist\latest.json`.
3. Upload the installer, then `latest.json`, to
   `/var/www/redvaultmessages/desktop/` on the droplet (`sudo mkdir -p` it the
   first time). The web deploy's `cp -r dist/*` never deletes that folder.

## What protects what

- **Relay-only calls** (`rtc/`): `TransportPolicy::Relay` only stops this side
  announcing its own address; libjuice still pairs its direct socket with whatever
  the other person lists, and a candidate's type is only a label they wrote. So
  remote SDP is filtered (`rtc/src/relay_sdp.cpp`) and libjuice is patched at
  configure time (`rtc/patch_relay_only.cmake`) so a relay-only agent has no direct
  pair at all. The patch is in the build, not a fork: a libdatachannel bump that
  moves those lines stops the configure. `tests/rtc_relay_test.cpp` checks this
  against a real TURN server on loopback, watching which port packets arrive from.
  That is not a packet capture of a real TURN call.
- **The PIN lock** (`app/src/app_lock.cpp`) is a screen lock for an unattended,
  running app. Its PIN and wrong-try count live in memory for the session, and a
  stored PIN that goes missing or changes ends the session. It doesn't stop a
  process running as the same user, or a compromised OS: those can read this app's
  memory, keys included, or send it input.
- **Received files** open with one click only if they're pictures, audio, video,
  plain text or PDF (`model::file_message::opensInertly`). Everything else is
  save-only, with the Mark of the Web on Windows. What a viewer does with a
  malformed picture or PDF is out of this app's hands.
- **Files on Linux**: the app runs under `umask 077`, tightens its cache, data
  and settings folders at startup (leftovers from a crash or an older version),
  and the core makes its own folders 0700 and files 0600 whatever the umask.
- **Protocol 1 keys** (`core/src/data/key_trust.cpp`, port of `ui/src/lib/keyTrust.ts`):
  a direct chat's key is pinned the first time and only epoch 0 is ever opened; a
  group's key needs a member's signed notice (issuer and this account named, a new
  epoch only from someone in the last verified list, no rollback, an absent notice
  after one was seen is a failure). A key that fails is dropped, nothing can be
  sent, and the chat says why. Pins live in local settings, so this computer starts
  again from trust on first use if they're cleared. Ids are lowercased at the API
  boundary and again before any pin, so capitals can't earn a second one. Nothing
  is sealed to anyone (group create, add, approve, rekey, accepting a request,
  starting a chat, vault and Files shares) while their public key differs from the
  one this computer pinned. Pins keep older epochs' commitments, a pinned signer
  stays one whatever the server answers, and a pinned chat is never treated as a
  community. A notice names the last verified members, not the server's list; a
  link joiner checks the latest notice first and publishes nothing if no member who
  held the key signed it.
- **Vault items** (`core/src/data/vault_repository.cpp`): `boundId` and `rev` sit
  inside the sealed data. An item in another record's place, or older than this
  computer has seen (kept per account in local settings), is left out and counted.
  Items are bound at their next save, new ones in a second write, and nothing is
  rewritten in the background. Shared copies keep their own revision.
- **libsodium**: CVE-2025-69277 (`crypto_core_ed25519_is_valid_point`) is fixed
  upstream in 1.0.21, which Windows requires (MSYS2 ships 1.0.22). Debian and
  Ubuntu keep the version at 1.0.18 and backport under it (Ubuntu 26.04's
  1.0.18-2 has the fix), so on Linux the build can only print what it found;
  check the changelog of the package that goes into the AppImage. No code here
  calls that function.

## Trying it against a real server

`redvault_cli` uses the real network stack. Each run signs in fresh and signs
out at the end; nothing is saved to disk.

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
.\build\debug\cli\redvault_cli.exe ping              # no account needed
.\build\debug\cli\redvault_cli.exe chats <handle>    # list chats
.\build\debug\cli\redvault_cli.exe read <handle> 1   # read chat 1
.\build\debug\cli\redvault_cli.exe send <handle> 1 hello from the desktop
.\build\debug\cli\redvault_cli.exe sendfile <handle> 1 C:\path\to\photo.jpg
.\build\debug\cli\redvault_cli.exe download <handle> 1 .\downloads
```

It talks to the production API by default; use `--api URL` or
`REDVAULT_API_BASE` for another server (e.g. `http://localhost:3000/api/`).

## Roadmap

1. **Crypto core + compat tests** — done.
2. **API layer** — done. `net::` (URL parsing, in-memory `SessionCookieJar`,
   libcurl `CurlTransport` that streams file bodies), `api::` (`ApiClient`
   for every `ApiService.kt` endpoint except push, DTOs mirroring `Dtos.kt`),
   `model::` (`RichMessage` replies/reactions and `FileMessage` descriptors,
   byte-identical to web/Android), and `data::` repositories: auth,
   conversations (decrypt, receipts, send, stage-3 verification), friends,
   groups and files (encrypted upload/download, proxy and direct).
3. **Qt 6 / QML shell** — done. Sign-in with two-factor, registration (with
   the one-time recovery key) and recovery; chat list and threads (replies,
   reactions, receipts, stage-3 verification); files (attach, open, save:
   programs and scripts can only be saved, and every decrypted file gets the
   Mark of the Web); friends and requests; groups (create, rename, members,
   roles, leave); settings (read receipts, friend requests, password,
   recovery key, two-factor) and safety-number verification.
4. **Desktop specifics** — done: tray icon and notifications, screen-capture
   exclusion, PIN app lock, installer. Profile photos (shown everywhere,
   fetched with the session cookie; upload strips EXIF) and QR codes (two-factor
   setup, and safety numbers a phone can scan) are in. Still open: code signing
   (needs a certificate).

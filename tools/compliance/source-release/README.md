# RedVault Messages apps

Source code of the RedVault Messages apps: an end-to-end encrypted messenger.

| Folder | What it is | Build instructions |
|---|---|---|
| `ui/` | Web app (React, Vite) | [ui/README.md](ui/README.md) |
| `desktop/` | Desktop app for Windows and Linux (C++, Qt 6) | [desktop/README.md](desktop/README.md) |
| `android/` | Android app (Kotlin, Jetpack Compose) | [android/README.md](android/README.md) |
| `crypto-core/` | Shared end-to-end encryption library (Rust), used by all three apps | [crypto-core/README.md](crypto-core/README.md) |
| `tor-core/` | Optional Tor routing (Rust, Arti), used by desktop and Android | [tor-core/README.md](tor-core/README.md) |
| `legal/` | Terms of Service and Privacy Policy shown in the apps | |

The server these apps talk to is not part of this repository.

## Licence

GNU Affero General Public License, version 3 only (AGPL-3.0-only). See
[LICENSE](LICENSE) and [NOTICE.md](NOTICE.md). Third-party components and
their licences are listed in [THIRD_PARTY.md](THIRD_PARTY.md). The RedVault
name and logo are covered by [TRADEMARKS.md](TRADEMARKS.md), not the AGPL.

## Building your own copy

- **Android push notifications** use Firebase. This repository has no
  `android/app/google-services.json`; without it the app builds and runs, but
  without push. Add your own Firebase project's file to enable push.
- **Signing**: release builds are signed with your own keys. Our signing keys
  are not part of the source.
- Each release tag (for example `android-v0.5.0`) matches exactly the source
  of the build with that version.

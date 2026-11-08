# Desktop: per-device keys (protocol 3) with Signal sessions

Milestone 5 of `docs/multi-device-threat-model.md`, plus the chat path of its
section 9, and step 1 of `docs/signal-integration/07-step1-default-path.md`
(Signal as the default and only path). Compiled in by default
(`REDVAULT_E2E_V3` and `REDVAULT_E2E_SIGNAL` are `ON`; needs Rust/cargo; turn
them off only for a test build), and inert unless the server answers
`/e2e/v3/config` with `enabled: true` (`E2E_V3_ENABLED`).

## Step 1: the default path

* **Device keys without asking.** `DeviceKeysSession::ensureDeviceKeys()` runs
  after sign-in, on unlock and every 6 hours. No signed list on the account:
  this computer is set up as the first device (the account key vouches for
  it), then Signal is turned on (`maintainSignal`), and a one-time banner
  offers to create a recovery key. A trusted computer gets `maintainSignal`.
  A computer that isn't in the account's list is never added: a banner stays
  until it is linked in Settings > Devices & encryption. Failures are silent
  and retried; only 404/503 from the config route means "off".
* **Chats decide.** One-to-one chats and plain groups use device keys when
  every other member has a signed list, or once pinned (first device-key
  message). Otherwise the older protocol, only while unpinned. While it isn't
  known yet, or when they all have lists but this computer isn't linked,
  nothing is sent (`chatMode` "checking" / "link"; ChatSession refuses with
  the reason). Only the open chat is decided this way: a send into another,
  unpinned chat (a forward, a story reply) follows its pin alone.
  Communities, channels, Stories and calls are unchanged.
* **No way back.** "Use device keys" and "Use the older protocol" are gone, as
  is "Turn off Signal". In a device-key chat there is no reply, forward, pin,
  poll, disappearing timer or multi-select (step 2).
* **Rollback detection** (`device_store.hpp`): every save stores
  `counter = previous + 1` with the engine state, under an HMAC-SHA256 keyed
  from the storage key, then the same counter in its own vault entry
  (`<user>/<device>/counter`: its own DPAPI blob, Secret Service item or 0600
  file). Lower on load: the state is refused, the computer is "not linked",
  and a local event is recorded (Settings shows it). Equal: fine. Higher (a
  crash between the two writes): accepted, the vault's copy raised. Missing:
  initialised only for a state saved before counters; a state that carries a
  counter is refused without it (the entry is written when the device is
  made, so deleting it can't hide a restore). Limit: restoring *both* the state and the vault entry (a
  full-disk image) isn't detected; that needs a hardware counter.

## What is where

| Part | Where |
|---|---|
| Engine (crypto-core) through its C ABI, built by cargo into the build tree | `e2ev3/CMakeLists.txt`, `crypto-core/include/redvault_e2ee.h` |
| Key storage: Windows DPAPI, Linux Secret Service (libsecret, `dlopen`ed), user-only file fallback | `e2ev3/.../key_vault.hpp`, `device_store.hpp` |
| Device logic (first device, new device, list publishing with compare-and-set retry, revoke, recovery key, safety number) | `e2ev3/.../device.hpp` |
| Pairing by comparing codes (SAS) | `e2ev3/.../pairing.hpp` |
| Server routes | `e2ev3/.../v3_server.hpp` (`HttpV3Server`; an interface so tests use a fake) |
| Chat path: envelopes (text, file, reaction, edit, delete), fold, encrypted local history with erase-on-delete, pins, 1:1, groups, receive loop | `e2ev3/.../chat.hpp` |
| Settings > Devices & encryption | `app/src/device_keys_session.*`, `app/qml/DeviceKeysSection.qml`, device-key number in `SafetyNumberDialog.qml` |
| Tests | `tests/e2ev3_test.cpp` (real engine, fake server), `tests/e2ev3_content_test.cpp` (shared vectors, erase against the real history file) |
| End to end against the real server | `tools/v3_probe/` (`redvault_v3_probe` + `run.mjs`) |

## Key storage and its honest limits

* **Windows:** `CryptProtectData` for the current user, with the device id as
  extra entropy. Anything running as the same Windows user can unprotect it.
  No TPM.
* **Linux:** the Secret Service if a keyring answers; otherwise a file only the
  user can read (0600). Settings says plainly when it is the file.
* The engine state (and the chat history, under a key derived from the
  storage key) are encrypted files; the storage key is never written
  unprotected. "Delete device keys" removes the key, state and history.
* A lost key (reset Windows profile, new keyring) loses the device, not the
  account: pair again or use the recovery key.

## Not done / not verified

* Pairing by scanning a QR code: only comparing codes is built (no QR scanner
  in the desktop app). The engine supports `approveDevice` with `qr`.
* Chat screen (wired): in a one-to-one or group chat a bar says how the chat
  is encrypted (see Step 1 above). In device-key mode the normal
  message list is replaced by the device-key history, sends go through
  `chat.hpp`, and the first send pins the chat. Messages arrive via the server's `v3-mailbox` live event or a
  5 s (chat open) / 30 s timer. Device-key messages in a chat that is not open
  show a shield badge with a count in the chat list and one content-free
  desktop notification ("N new messages"). Device-key chats can react, edit
  and delete your own messages for everyone, and send and open files (9.3,
  shared cases in `tests/vectors/v3_chat_content_vectors.json`): a file is
  encrypted, uploaded and released by `FileRepository::uploadForDeviceKeys`,
  and only the envelope carries its key. A delete rewrites the history file
  without the text and file key, but that says nothing about old copies an SSD
  or a backup may keep; those stay encrypted under the history key. Not done:
  replies, forwards, pins and polls (hidden in device-key mode), and picture
  previews (a picture is a file row). The QML has been compiled, not driven by
  hand on screen.
* Groups, the group routes and the live event were run against the real server
  code of the web/server session's branch (`claude/v3-web-server-1676c6`,
  extracted with `git archive`; run.mjs with `V3_SERVER_TREE=<dir>`): 49/49.
  Not in this branch's own `server/`.
* The Settings panel was compiled (QML cache) but not driven by hand on
  screen.
* Linux: library/tests built and run in WSL; no desktop session, so the
  libsecret path was not exercised (file fallback only).

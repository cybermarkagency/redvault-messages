# Desktop: per-device keys (protocol 3), development preview

Milestone 5 of `docs/multi-device-threat-model.md`, plus the chat path of its
section 9. **Development only.** Compiled in only with
`-DREDVAULT_E2E_V3=ON` (needs Rust/cargo; off by default), and inert unless the
server answers `/e2e/v3/config` with `enabled: true` (`E2E_V3_ENABLED`, off by
default). No real account is migrated, nothing is deployed or pushed.

## What is where

| Part | Where |
|---|---|
| Engine (crypto-core) through its C ABI, built by cargo into the build tree | `e2ev3/CMakeLists.txt`, `crypto-core/include/redvault_e2ee.h` |
| Key storage: Windows DPAPI, Linux Secret Service (libsecret, `dlopen`ed), user-only file fallback | `e2ev3/.../key_vault.hpp`, `device_store.hpp` |
| Device logic (first device, new device, list publishing with compare-and-set retry, revoke, recovery key, safety number) | `e2ev3/.../device.hpp` |
| Pairing by comparing codes (SAS) | `e2ev3/.../pairing.hpp` |
| Server routes | `e2ev3/.../v3_server.hpp` (`HttpV3Server`; an interface so tests use a fake) |
| Chat path: envelopes (text, file, reaction, edit, delete), fold, encrypted local history with erase-on-delete, pins, 1:1, groups, receive loop | `e2ev3/.../chat.hpp` |
| Settings > Device keys (preview) | `app/src/device_keys_session.*`, `app/qml/DeviceKeysSection.qml`, device-key number in `SafetyNumberDialog.qml` |
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
* Chat screen (wired): in a one-to-one or group chat a trusted
  device sees a "Device keys (preview)" bar. It says whether the other side
  (a group: any other member) has device keys, and offers "Use device keys"
  (with a confirmation) only when they do. In device-key mode the normal
  message list is replaced by the device-key history, sends go through
  `chat.hpp`, and the first send pins the chat; "Use the older protocol"
  unpins it. Messages arrive via the server's `v3-mailbox` live event or a
  5 s (chat open) / 30 s timer. Device-key messages in a chat that is not open
  show a shield badge with a count in the chat list and one content-free
  desktop notification ("N new messages"). Device-key chats can react, edit
  and delete your own messages for everyone, and send and open files (9.3,
  shared cases in `tests/vectors/v3_chat_content_vectors.json`): a file is
  encrypted, uploaded and released by `FileRepository::uploadForDeviceKeys`,
  and only the envelope carries its key. A delete rewrites the history file
  without the text and file key, but that says nothing about old copies an SSD
  or a backup may keep; those stay encrypted under the history key. Not done:
  replies, forwards, pins and polls (disabled in device-key mode), and picture
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

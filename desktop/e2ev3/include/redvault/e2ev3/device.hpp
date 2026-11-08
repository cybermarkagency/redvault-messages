// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "redvault/crypto/bytes.hpp"
#include "redvault/e2ev3/device_store.hpp"
#include "redvault/e2ev3/engine.hpp"
#include "redvault/e2ev3/v3_server.hpp"

// This computer's protocol-3 device and what it does with the server
// (docs/multi-device-threat-model.md): a port of ui/src/lib/e2eV3/device.ts.
// A preview: nothing here runs unless the server has E2E_V3_ENABLED. Protocol 1
// is untouched. Every call blocks (network, Argon2-free but still disk and
// crypto): run it off the UI thread.

namespace redvault::e2ev3 {

/// The engine refused (a stable code such as "no_session", "rollback", "bad_device_list") or the input was unusable.
class EngineError : public std::runtime_error {
public:
    EngineError(std::string code, const std::string& message) : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

/// The pieces every operation needs: where the device lives and the server it talks to.
struct Backend {
    std::shared_ptr<DeviceStore> store;
    std::shared_ptr<V3Server> server;
};

/// This computer's device for one account. Every engine call saves the new state before returning.
class V3Device {
public:
    /// The device this computer keeps for `userId`, if any.
    static std::unique_ptr<V3Device> open(const std::string& userId, const Backend& backend);
    /// Makes the engine state for a new device record (createDevice or createAccount) and keeps it. `fields` are added to the request.
    static std::unique_ptr<V3Device> create(const std::string& userId, const std::string& op, const Json& fields, const Backend& backend,
                                            Json* response = nullptr);

    ~V3Device();
    V3Device(const V3Device&) = delete;
    V3Device& operator=(const V3Device&) = delete;

    const std::string& userId() const { return userId_; }
    const std::string& deviceId() const { return deviceId_; }
    V3Server& server() const { return *backend_.server; }
    const Backend& backend() const { return backend_; }

    /// One engine call on this device; a successful answer's new state is saved first. Returns the answer even when it is an error ("ok": false).
    Json op(const std::string& op, const Json& fields = Json::object());
    /// Like op(), but an error answer throws EngineError.
    Json must(const std::string& op, const Json& fields = Json::object());

    Json identity();
    /// A 32-byte key for `context`, derived from the device's storage key (BLAKE2b, keyed): for data stored beside the engine state, such as chat history.
    crypto::Bytes deriveKey(const std::string& context) const;
    /// Publishes this device's public keys: identity, signed one-time keys, the signed fallback key.
    void publishKeys(int count = 20);
    /// Forgets this device here and in the server's directory.
    void forget();

private:
    V3Device(std::string userId, std::string deviceId, std::string storageKey, std::string state, Backend backend);

    std::string userId_;
    std::string deviceId_;
    std::string storageKey_;
    std::string state_;
    Backend backend_;
    std::mutex mutex_;
};

/// Throws EngineError when the answer isn't ok.
Json requireOk(Json response, const std::string& what);

/**
 * Moves an existing account to per-device identities from this computer: a new
 * account key, certified by the account's legacy signing key (migration 030) so
 * contacts who pinned that key accept it. The legacy key stays where it is
 * (protocol 1 keeps working). If any step fails, everything this did is undone.
 */
std::unique_ptr<V3Device> setUpFirstDevice(const std::string& userId, crypto::ByteView legacyPrivateKey, const Backend& backend);

/// A computer joining an account that already has trusted devices: its own keys, waiting to be linked.
std::unique_ptr<V3Device> startNewDevice(const std::string& userId, const Backend& backend);

/**
 * Puts a list this device signed. If another trusted device changed the list
 * first, adopts that list (it's signed by the same account key) and redoes the
 * change once on top of it.
 */
Json publishList(V3Device& device, const std::function<Json()>& change, const Json& rotationProof = nullptr);

/// Removes another device of this account from the signed list; contacts are told by the next list they check.
void revokeDevice(V3Device& device, const std::string& deviceId);

// --- Recovery ---

/// The recovery key as people see it: 64 hex digits in groups of four.
std::string formatRecoveryKey(crypto::ByteView bytes);
/// Nothing unless `text` holds exactly 64 hex digits (separators ignored).
std::optional<crypto::Bytes> parseRecoveryKey(const std::string& text);

/**
 * A new recovery key and the account key (and legacy key) encrypted under it,
 * stored on the server, which never sees the key. Returns the key, to show
 * once; nothing on this computer keeps it.
 */
std::string createRecoveryBackup(V3Device& device);

/// With no other device left: a new device here, restored from the recovery backup.
std::unique_ptr<V3Device> restoreWithRecoveryKey(const std::string& userId, const std::string& recoveryKeyText, const Backend& backend);

// --- Contacts' lists and safety numbers ---

struct VerifiedDevices {
    Json devices = Json::array();
    Json response;  // the engine's verifyDeviceList answer, null when the person has no list
    Json pendingReset;
};

/// The devices of `userId` from a list this device verified (contacts: pinned key; own: same account key).
VerifiedDevices verifiedDevices(V3Device& device, const std::string& userId, const std::string& conversationId = {},
                                const std::string& legacySigningKey = {});

/**
 * The safety number over this account's and the contact's account keys
 * (Signal's numeric fingerprint construction, crypto-core/src/identity.rs), as
 * 12 groups of 5 digits, or nothing while the contact has no device keys.
 * Comparing it proves both apps see the same keys; it says nothing about who
 * the person is.
 */
std::optional<std::vector<std::string>> deviceKeySafetyNumber(V3Device& device, const std::string& contactUserId,
                                                              const std::string& conversationId = {});

/// Everything a Verify screen shows for the device-key safety number.
struct DeviceKeySafety {
    std::vector<std::string> groups;  // 12 groups of 5 digits
    std::string qrText;               // "RVDK1:..." for the other person's camera
    std::string accountKey;           // the contact's account key the number is over
    bool verified = false;            // this account key was verified on this device
    bool keyChanged = false;          // the contact's signed list has a new account key, not trusted yet
};

/**
 * The device-key safety number with a contact, or nothing while either has no
 * device keys. When the contact's account key changed, this is the number over
 * the NEW key (from a list that key signed) with keyChanged set, so the person
 * can compare it before accepting the key with acceptContactKey.
 */
std::optional<DeviceKeySafety> deviceKeySafety(V3Device& device, const std::string& contactUserId, const std::string& conversationId = {});

/// The person compared the number for `accountKey` and marks it verified (or takes that back).
void setContactVerified(V3Device& device, const std::string& contactUserId, const std::string& accountKey, bool verified);

/// Accepts a contact's new account key (after a key change) and re-pins their list; `verified` when
/// the person compared the new number first.
void acceptContactKey(V3Device& device, const std::string& contactUserId, const std::string& accountKey, bool verified,
                      const std::string& conversationId = {});

/// The other person's code read by a camera: true when it matches (then `accountKey` is verified, and
/// accepted as their new key when it changed).
bool verifyContactQr(V3Device& device, const std::string& contactUserId, const std::string& accountKey, const std::string& qrText,
                     const std::string& conversationId = {});

// --- rv-signal-v1 (docs/signal-integration/): development only ---

/// Whether this build's engine has Signal sessions (crypto-core built with the `signal` feature).
bool engineHasSignal();

/**
 * Keeps this device's Signal side in order, if both the build and the server
 * (`config`, from /e2e/v3/config) have Signal:
 *   - turns it on once: the device's Signal identity and certificate;
 *   - declares the device's certificate in the account's signed list, which
 *     needs the account key (every linked device holds it);
 *   - keeps at least `low` unused one-time keys of each kind on the server.
 * Returns "off" (no Signal in the build or on the server), "enabled", "declared"
 * or "ok". Safe to call on every refresh.
 */
std::string maintainSignal(V3Device& device, const Json& config, int low = 10);

// --- One-to-one messages, with copies for this account's other devices ---

/**
 * Sends `text` to every device of the contact and every other device of this
 * account, claiming one-time keys only where there's no session yet. Returns
 * how many device copies were delivered to the server.
 *
 * The engine picks the protocol for each device: rv-signal-v1 when both
 * devices speak it, Olm otherwise. It asks for a Signal bundle
 * ("no_signal_session") or an Olm one-time key ("no_session") as needed. A
 * device that speaks Signal but has no keys on the server gets nothing, never
 * an Olm copy.
 */
int sendText(V3Device& device, const std::string& conversationId, const std::string& contactUserId, const std::string& text,
             int* unreachable = nullptr);

/**
 * Which protocol a one-to-one chat's messages use (M5), worked out by the engine from the
 * verified lists: "signal" (every device: the contact's and this account's other devices),
 * "olm" (none), "mixed" (some), or "" while the contact has no device keys.
 */
std::string chatProtocol(V3Device& device, const std::string& conversationId, const std::string& contactUserId);

/// Turns Signal off for one of this account's devices, in the signed list (needs the account key). Contacts are told.
void turnOffSignal(V3Device& device, const std::string& deviceId);

struct Received {
    std::string id;
    std::string senderUserId;
    std::string senderDeviceId;
    std::string conversationId;  // empty for account-level messages (device links)
    std::string kind;
    std::optional<std::string> text;
    std::string error;  // empty when it opened
};

/**
 * Reads this device's mailbox. Each sender's list is verified first. Items are
 * acknowledged once handled, even one that can't be decrypted (a duplicate, or
 * not for this device), so one bad item never blocks the rest; only items
 * whose sender's identity changed are kept for later.
 */
std::vector<Received> receive(V3Device& device);

}  // namespace redvault::e2ev3

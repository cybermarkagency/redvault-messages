#include "redvault/e2ev3/device.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "redvault/api/api_error.hpp"
#include <sodium.h>

#include "redvault/crypto/base64.hpp"

namespace redvault::e2ev3 {

namespace {

std::string errorCode(const Json& response) {
    return response.value("error", std::string("engine_error"));
}

std::string errorMessage(const Json& response, const std::string& fallback) {
    return response.value("message", fallback);
}

std::string stringOr(const Json& json, const char* key, const std::string& fallback = {}) {
    const auto it = json.find(key);
    return it != json.end() && it->is_string() ? it->get<std::string>() : fallback;
}

bool isStaleList(const api::ApiError& error) {
    return error.code() == "stale_device_list";
}

}  // namespace

Json requireOk(Json response, const std::string& what) {
    if (!response.value("ok", false)) throw EngineError(errorCode(response), errorMessage(response, what));
    return response;
}

V3Device::V3Device(std::string userId, std::string deviceId, std::string storageKey, std::string state, Backend backend)
    : userId_(std::move(userId)), deviceId_(std::move(deviceId)), storageKey_(std::move(storageKey)), state_(std::move(state)), backend_(std::move(backend)) {}

V3Device::~V3Device() {
    // The storage key is only in memory while the device is open.
    volatile char* wipe = storageKey_.data();
    for (std::size_t i = 0; i < storageKey_.size(); ++i) wipe[i] = 0;
}

std::unique_ptr<V3Device> V3Device::open(const std::string& userId, const Backend& backend) {
    auto stored = backend.store->load(userId);
    if (!stored || !stored->state) return nullptr;
    return std::unique_ptr<V3Device>(new V3Device(userId, stored->deviceId, stored->storageKey, *stored->state, backend));
}

std::unique_ptr<V3Device> V3Device::create(const std::string& userId, const std::string& op, const Json& fields, const Backend& backend,
                                           Json* response) {
    OpenDevice stored = backend.store->create(userId);
    Json request = {{"op", op}, {"userId", userId}, {"deviceId", stored.deviceId}, {"pickleKey", stored.storageKey}};
    for (auto it = fields.begin(); it != fields.end(); ++it) request[it.key()] = it.value();
    Json answer = runEngine(request);
    if (!answer.value("ok", false) || !answer.contains("state")) {
        backend.store->wipe(userId);
        throw EngineError(errorCode(answer), errorMessage(answer, "the engine refused"));
    }
    const std::string state = answer.at("state").get<std::string>();
    try {
        backend.store->saveState(userId, stored.deviceId, state);
    } catch (...) {
        backend.store->wipe(userId);
        throw;
    }
    if (response) *response = std::move(answer);
    return std::unique_ptr<V3Device>(new V3Device(userId, stored.deviceId, stored.storageKey, state, backend));
}

Json V3Device::op(const std::string& op, const Json& fields) {
    std::lock_guard lock(mutex_);
    Json request = {{"op", op}, {"state", state_}, {"pickleKey", storageKey_}};
    for (auto it = fields.begin(); it != fields.end(); ++it) request[it.key()] = it.value();
    Json answer = runEngine(request);
    if (answer.value("ok", false) && answer.contains("state") && answer.at("state").is_string()) {
        // Saved before anything this call produced can be sent: a crash never reuses a message key.
        const std::string state = answer.at("state").get<std::string>();
        backend_.store->saveState(userId_, deviceId_, state);
        state_ = state;
    }
    return answer;
}

Json V3Device::must(const std::string& opName, const Json& fields) {
    return requireOk(op(opName, fields), opName);
}

Json V3Device::identity() {
    return must("identity").at("identity");
}

crypto::Bytes V3Device::deriveKey(const std::string& context) const {
    const crypto::Bytes key = crypto::base64Decode(storageKey_);
    crypto::Bytes out(32);
    crypto_generichash(out.data(), out.size(), reinterpret_cast<const unsigned char*>(context.data()), context.size(), key.data(), key.size());
    return out;
}

void V3Device::publishKeys(int count) {
    const Json made = must("prekeys", Json{{"count", count}});
    backend_.server->registerDevice(made.at("upload"));
}

void V3Device::forget() {
    try {
        backend_.server->removeDevice(deviceId_);
    } catch (const std::exception&) {
        // Gone already, or no connection: the local keys go either way.
    }
    backend_.store->wipe(userId_);
}

std::unique_ptr<V3Device> setUpFirstDevice(const std::string& userId, crypto::ByteView legacyPrivateKey, const Backend& backend) {
    Json response;
    auto device = V3Device::create(userId, "createAccount", Json{{"legacyAccountKey", crypto::base64Encode(legacyPrivateKey)}}, backend, &response);
    try {
        device->publishKeys();
        backend.server->putDeviceList(response.at("deviceList"), nullptr);
        return device;
    } catch (...) {
        device->forget();
        throw;
    }
}

std::unique_ptr<V3Device> startNewDevice(const std::string& userId, const Backend& backend) {
    auto device = V3Device::create(userId, "createDevice", Json::object(), backend);
    try {
        device->publishKeys();
    } catch (...) {
        device->forget();
        throw;
    }
    return device;
}

Json publishList(V3Device& device, const std::function<Json()>& change, const Json& rotationProof) {
    Json made = change();
    try {
        device.server().putDeviceList(made.at("deviceList"), rotationProof);
    } catch (const api::ApiError& error) {
        if (!isStaleList(error)) throw;
        const Json current = device.server().getDeviceList(device.userId()).at("deviceList");
        device.must("verifyDeviceList", Json{{"deviceList", current}});
        made = change();
        device.server().putDeviceList(made.at("deviceList"), rotationProof);
    }
    return made;
}

void revokeDevice(V3Device& device, const std::string& deviceId) {
    publishList(device, [&] { return device.must("revokeDevice", Json{{"deviceId", deviceId}}); });
    try {
        device.server().removeDevice(deviceId);
    } catch (const std::exception&) {
    }
}

// --- Recovery ---

std::string formatRecoveryKey(crypto::ByteView bytes) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0 && i % 2 == 0) out += '-';
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

std::optional<crypto::Bytes> parseRecoveryKey(const std::string& text) {
    std::string hex;
    for (const char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) hex += c;
    }
    if (hex.size() != 64) return std::nullopt;
    crypto::Bytes out;
    for (std::size_t i = 0; i < hex.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

std::string createRecoveryBackup(V3Device& device) {
    crypto::SecretBytes key = crypto::SecretBytes::copyOf(crypto::randomBytes(32));
    const Json made = device.must("exportAccountBackup", Json{{"recoveryKey", crypto::base64Encode(key.view())}});
    device.server().putRecoveryBackup(made.at("backup").get<std::string>(), made.at("signature").get<std::string>());
    return formatRecoveryKey(key.view());
}

std::unique_ptr<V3Device> restoreWithRecoveryKey(const std::string& userId, const std::string& recoveryKeyText, const Backend& backend) {
    auto key = parseRecoveryKey(recoveryKeyText);
    if (!key) throw EngineError("bad_recovery_key", "A recovery key has 64 letters and digits.");
    const Json backup = backend.server->getRecoveryBackup();
    const Json current = backend.server->getDeviceList(userId).at("deviceList");
    auto device = startNewDevice(userId, backend);
    try {
        const Json restored = device->must("restoreAccountBackup", Json{{"backup", backup.at("backup")},
                                                                       {"recoveryKey", crypto::base64Encode(*key)},
                                                                       {"deviceList", current}});
        backend.server->putDeviceList(restored.at("deviceList"), nullptr);
    } catch (...) {
        device->forget();
        std::fill(key->begin(), key->end(), 0);
        throw;
    }
    std::fill(key->begin(), key->end(), 0);
    return device;
}

// --- Contacts' lists and safety numbers ---

VerifiedDevices verifiedDevices(V3Device& device, const std::string& userId, const std::string& conversationId, const std::string& legacySigningKey) {
    const Json view = device.server().getDeviceList(userId, conversationId);
    VerifiedDevices out;
    out.pendingReset = view.value("pendingReset", Json(nullptr));
    const auto list = view.find("deviceList");
    if (list == view.end() || list->is_null()) return out;
    Json fields = {{"deviceList", *list}};
    if (!legacySigningKey.empty()) fields["legacySigningKey"] = legacySigningKey;
    out.response = device.op("verifyDeviceList", fields);
    if (!out.response.value("ok", false)) {
        throw EngineError(stringOr(out.response, "error", "bad_device_list"), errorMessage(out.response, "the device list doesn't verify"));
    }
    out.devices = out.response.value("devices", Json::array());
    return out;
}

std::optional<std::vector<std::string>> deviceKeySafetyNumber(V3Device& device, const std::string& contactUserId, const std::string& conversationId) {
    const VerifiedDevices known = verifiedDevices(device, contactUserId, conversationId);
    if (known.devices.empty()) return std::nullopt;
    const Json fingerprint = device.must("fingerprint", Json{{"userId", contactUserId}});
    return fingerprint.at("safetyNumber").get<std::vector<std::string>>();
}

std::optional<DeviceKeySafety> deviceKeySafety(V3Device& device, const std::string& contactUserId, const std::string& conversationId) {
    const Json view = device.server().getDeviceList(contactUserId, conversationId);
    const auto list = view.find("deviceList");
    if (list == view.end() || list->is_null()) return std::nullopt;
    const Json verified = device.op("verifyDeviceList", Json{{"deviceList", *list}});
    Json fields = {{"userId", contactUserId}};
    if (!verified.value("ok", false)) {
        const std::string code = stringOr(verified, "error", "bad_device_list");
        // A key change: the list verified under its own (new) key, which isn't the pinned one.
        if (code != "account_key_changed") throw EngineError(code, errorMessage(verified, "the device list doesn't verify"));
        fields["accountKey"] = list->at("accountKey");
    } else if (verified.value("devices", Json::array()).empty()) {
        return std::nullopt;
    }
    const Json fingerprint = device.must("fingerprint", fields);
    DeviceKeySafety out;
    out.groups = fingerprint.at("safetyNumber").get<std::vector<std::string>>();
    out.qrText = stringOr(fingerprint, "qrText", "");
    out.accountKey = stringOr(fingerprint, "accountKey", "");
    out.verified = fingerprint.value("verified", false);
    out.keyChanged = fingerprint.value("keyChanged", false);
    return out;
}

void setContactVerified(V3Device& device, const std::string& contactUserId, const std::string& accountKey, bool verified) {
    device.must("setContactVerified", Json{{"userId", contactUserId}, {"accountKey", accountKey}, {"verified", verified}});
}

void acceptContactKey(V3Device& device, const std::string& contactUserId, const std::string& accountKey, bool verified,
                      const std::string& conversationId) {
    device.must("trustAccountKey", Json{{"userId", contactUserId}, {"accountKey", accountKey}, {"verified", verified}});
    // Re-pin their current list under the accepted key (sessions with the old key's devices are gone).
    verifiedDevices(device, contactUserId, conversationId);
}

bool verifyContactQr(V3Device& device, const std::string& contactUserId, const std::string& accountKey, const std::string& qrText,
                     const std::string& conversationId) {
    const Json result = device.must("verifyFingerprintQr", Json{{"userId", contactUserId}, {"accountKey", accountKey},
                                                                {"qrText", qrText}, {"acceptNewKey", true}});
    if (result.value("acceptedNewKey", false)) verifiedDevices(device, contactUserId, conversationId);
    return result.value("verified", false);
}

// --- rv-signal-v1 ---

bool engineHasSignal() {
    const Json version = runEngine(Json{{"op", "version"}});
    return version.contains("signal") && version.at("signal").is_object();
}

std::string maintainSignal(V3Device& device, const Json& config, int low) {
    if (!config.value("signal", false) || !engineHasSignal()) return "off";
    // Turned off for this device (its capability left the signed list): stays off until the person turns it on again.
    if (device.must("signalStatus").value("disabled", false)) return "off";
    std::string outcome = "ok";
    const Json status = device.server().signalStatus(device.deviceId());
    const Json enabled = device.must("signalEnable");
    const Json& certificate = enabled.at("certificate");
    if (!status.value("enabled", false)) {
        device.server().signalUpload(device.deviceId(), device.must("signalPrekeys", Json{{"count", 20}}).at("upload"));
        outcome = "enabled";
    } else if (status.value("preKeys", 0) < low || status.value("kyberPreKeys", 0) < low) {
        device.server().signalUpload(device.deviceId(), device.must("signalPrekeys", Json{{"count", 20}}).at("upload"));
    }
    // Declared in the signed list? Only a device holding the account key can sign it.
    const Json own = device.op("deviceList");
    if (own.value("ok", false) && own.value("trusted", false)) {
        bool declared = false;
        for (const Json& c : own.at("deviceList").value("capabilities", Json::array())) {
            if (c.value("deviceId", std::string()) == device.deviceId()) declared = true;
        }
        if (!declared) {
            publishList(device, [&] { return device.must("setSignalCapability", Json{{"certificate", certificate}}); });
            outcome = outcome == "enabled" ? "enabled" : "declared";
        }
    }
    return outcome;
}

std::string chatProtocol(V3Device& device, const std::string& conversationId, const std::string& contactUserId) {
    Json recipients = Json::array();
    for (const Json& d : verifiedDevices(device, contactUserId, conversationId).devices) recipients.push_back(Json{{"identity", d}});
    if (recipients.empty()) return "";
    for (const Json& d : verifiedDevices(device, device.userId()).devices) {
        if (d.value("deviceId", std::string()) != device.deviceId()) recipients.push_back(Json{{"identity", d}});
    }
    const Json answer = device.must("chatProtocols", Json{{"recipients", recipients}});
    std::size_t signal = 0;
    const Json protocols = answer.value("protocols", Json::array());
    for (const Json& p : protocols) {
        if (p.value("protocol", std::string()) == "rv-signal-v1") ++signal;
    }
    if (signal == 0) return "olm";
    return signal == protocols.size() ? "signal" : "mixed";
}

void turnOffSignal(V3Device& device, const std::string& deviceId) {
    publishList(device, [&] { return device.must("clearSignalCapability", Json{{"deviceId", deviceId}}); });
}

// --- One-to-one messages ---

int sendText(V3Device& device, const std::string& conversationId, const std::string& contactUserId, const std::string& text,
             int* unreachable) {
    std::vector<Json> recipients;
    for (const Json& d : verifiedDevices(device, contactUserId, conversationId).devices) recipients.push_back(d);
    for (const Json& d : verifiedDevices(device, device.userId()).devices) {
        if (d.value("deviceId", std::string()) != device.deviceId()) recipients.push_back(d);
    }
    const Json first = device.must("encryptMany", Json{{"plaintext", text}, {"recipients", [&] {
                                                         Json list = Json::array();
                                                         for (const Json& d : recipients) list.push_back(Json{{"identity", d}});
                                                         return list;
                                                     }()}});
    std::vector<Json> results = first.at("results").get<std::vector<Json>>();

    // The engine asks for what each device needs: an Olm one-time key, or a Signal bundle.
    std::vector<std::size_t> needOlm;
    std::vector<std::size_t> needSignal;
    for (std::size_t i = 0; i < recipients.size(); ++i) {
        const std::string code = results[i].value("error", std::string());
        if (code == "no_session") needOlm.push_back(i);
        if (code == "no_signal_session") needSignal.push_back(i);
    }
    if (!needOlm.empty() || !needSignal.empty()) {
        auto ask = [&](const std::vector<std::size_t>& which) {
            Json out = Json::array();
            for (std::size_t i : which) out.push_back({{"userId", recipients[i].at("userId")}, {"deviceId", recipients[i].at("deviceId")}});
            return out;
        };
        Json retryList = Json::array();
        std::vector<std::size_t> order;
        if (!needOlm.empty()) {
            const Json claimed = device.server().claimKeys(conversationId, ask(needOlm));
            for (std::size_t k = 0; k < needOlm.size(); ++k) {
                Json entry = {{"identity", recipients[needOlm[k]]}};
                const Json& c = claimed.at(k);
                const Json key = c.contains("oneTimeKey") && !c.at("oneTimeKey").is_null() ? c.at("oneTimeKey") : c.value("fallbackKey", Json(nullptr));
                if (!key.is_null()) entry["oneTimeKey"] = key;
                retryList.push_back(entry);
                order.push_back(needOlm[k]);
            }
        }
        if (!needSignal.empty()) {
            const Json claimed = device.server().signalClaim(conversationId, ask(needSignal));
            for (std::size_t k = 0; k < needSignal.size(); ++k) {
                const Json& c = claimed.at(k);
                retryList.push_back(Json{{"identity", recipients[needSignal[k]]},
                                         {"signalBundle", Json{{"certificate", c.value("certificate", Json(nullptr))}, {"bundle", c.value("bundle", Json(nullptr))}}}});
                order.push_back(needSignal[k]);
            }
        }
        const Json retry = device.must("encryptMany", Json{{"plaintext", text}, {"recipients", retryList}});
        const auto retried = retry.at("results").get<std::vector<Json>>();
        for (std::size_t k = 0; k < order.size(); ++k) results[order[k]] = retried.at(k);
    }
    Json delivered = Json::array();
    for (const Json& r : results) {
        if (r.contains("error") && !r.at("error").is_null()) {
            if (unreachable) ++*unreachable;
            continue;
        }
        if (!r.contains("message") || r.at("message").is_null()) continue;
        delivered.push_back({{"userId", r.at("userId")}, {"deviceId", r.at("deviceId")}, {"body", r.at("message").dump()}});
    }
    if (!delivered.empty()) device.server().sendMessages(device.deviceId(), conversationId, delivered);
    return static_cast<int>(delivered.size());
}

std::vector<Received> receive(V3Device& device) {
    // Errors about the sender's list, not the message: the item waits until the person accepts the change.
    // rv-signal-v1: a device's Signal identity changed or doesn't match its signed list. Held, never dropped,
    // so the person can compare safety numbers first (docs/signal-integration/, M5).
    static const std::set<std::string> hold = {"account_key_changed", "rollback", "equivocation", "uncertified_account_key", "bad_device_list",
                                               "identity_changed", "certificate_mismatch", "untrusted_identity"};
    const Json items = device.server().inbox(device.deviceId());
    std::vector<Received> out;
    Json handled = Json::array();
    struct Known {
        Json devices = Json::array();
        std::string error;
    };
    std::map<std::string, Known> lists;
    for (const Json& item : items) {
        const std::string senderUserId = item.at("senderUserId").get<std::string>();
        const std::string conversationId = stringOr(item, "conversationId");
        const std::string key = senderUserId + "/" + conversationId;
        auto found = lists.find(key);
        if (found == lists.end()) {
            Known known;
            try {
                known.devices = verifiedDevices(device, senderUserId, conversationId).devices;
            } catch (const EngineError& error) {
                // A device waiting for its link has no list of its own yet: the link message carries and checks one.
                if (error.code() == "not_linked" && senderUserId == device.userId()) {
                    const Json view = device.server().getDeviceList(device.userId());
                    const auto list = view.find("deviceList");
                    if (list != view.end() && list->is_object()) known.devices = list->value("devices", Json::array());
                } else {
                    known.error = error.code();
                }
            } catch (const api::ApiError& error) {
                known.error = error.code().empty() ? "unknown_sender" : error.code();
            }
            found = lists.emplace(key, std::move(known)).first;
        }
        const Known& known = found->second;
        const std::string senderDeviceId = item.at("senderDeviceId").get<std::string>();
        const Json* sender = nullptr;
        for (const Json& d : known.devices) {
            if (d.value("deviceId", std::string()) == senderDeviceId) sender = &d;
        }
        std::optional<Json> response;
        if (sender) {
            try {
                response = device.op("decrypt", Json{{"sender", *sender}, {"message", Json::parse(item.at("body").get<std::string>())}});
            } catch (const std::exception&) {
                response = Json{{"ok", false}, {"error", "decrypt_failed"}};
            }
        }
        std::string error;
        if (response) {
            if (!response->value("ok", false)) error = stringOr(*response, "error", "decrypt_failed");
        } else {
            error = known.error.empty() ? "unknown_sender" : known.error;
        }
        if (error.empty() || !hold.count(error)) handled.push_back(item.at("id"));
        Received r;
        r.id = item.at("id").get<std::string>();
        r.senderUserId = senderUserId;
        r.senderDeviceId = senderDeviceId;
        r.conversationId = conversationId;
        r.kind = response ? stringOr(*response, "kind", "message") : "message";
        if (response && response->value("ok", false) && response->contains("plaintext") && response->at("plaintext").is_string()) {
            r.text = response->at("plaintext").get<std::string>();
        }
        r.error = error;
        out.push_back(std::move(r));
    }
    if (!handled.empty()) device.server().ack(device.deviceId(), handled);
    return out;
}

}  // namespace redvault::e2ev3

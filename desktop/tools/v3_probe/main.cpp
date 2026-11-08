// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// A desktop computer for the end-to-end run against the real server
// (desktop/tools/v3_probe/run.mjs), development only. Reads one JSON command
// per line on stdin and answers one JSON line on stdout, driving exactly the
// library the Settings panel uses (redvault_e2ev3): the device store with this
// machine's key protection, the HTTP routes, pairing and one-to-one messages.
// It signs in to a throwaway dev API with a dev account's password; it never
// touches a real account. The engine's private values are never printed.
#include <iostream>
#include <memory>
#include <string>

#include "redvault/api/api_client.hpp"
#include "redvault/api/live_api.hpp"
#include <chrono>
#include "redvault/crypto/base64.hpp"
#include "redvault/e2ev3/chat.hpp"
#include "redvault/e2ev3/device.hpp"
#include "redvault/e2ev3/key_vault.hpp"
#include "redvault/e2ev3/pairing.hpp"
#include "redvault/net/cookie_jar.hpp"
#include "redvault/net/curl_transport.hpp"

namespace e2 = redvault::e2ev3;
using e2::Json;

namespace {

struct Probe {
    std::shared_ptr<redvault::api::ApiClient> api;
    e2::Backend backend;
    std::string userId;
    std::string legacyKey;  // base64: the dev account's protocol-1 key, which the real app has after signing in
    std::string protection;
    std::filesystem::path dataDir;
    std::unique_ptr<e2::V3Device> device;
    std::unique_ptr<e2::ChatHistory> history;
    std::unique_ptr<e2::NewDevicePairing> joining;
    std::unique_ptr<e2::TrustedPairing> answering;
};

Json codesJson(const std::vector<int>& codes) {
    return Json(codes);
}

Json handle(Probe& p, const Json& cmd) {
    const std::string name = cmd.at("cmd").get<std::string>();
    if (name == "init") {
        auto transport = std::make_shared<redvault::net::CurlTransport>();
        auto cookies = std::make_shared<redvault::net::SessionCookieJar>();
        p.api = std::make_shared<redvault::api::ApiClient>(cmd.at("baseUrl").get<std::string>(), transport, cookies);
        const Json login = p.api->call("POST", "auth/login", Json{{"username", cmd.at("username")}, {"password", cmd.at("password")}});
        p.userId = login.at("userId").get<std::string>();
        p.legacyKey = cmd.at("legacyKey").get<std::string>();
        const std::filesystem::path dir = cmd.at("dataDir").get<std::string>();
        p.dataDir = dir;
        std::shared_ptr<e2::KeyVault> vault;
        if (cmd.value("vault", std::string("default")) == "file") {
            vault = e2::makeFileVault(dir / "keys");
        } else {
            vault = e2::makeDefaultVault(dir / "keys");
        }
        p.protection = e2::describeProtection(vault->protection());
        p.backend.store = std::make_shared<e2::DeviceStore>(dir / "state", vault);
        p.backend.server = std::make_shared<e2::HttpV3Server>(p.api);
        p.device = e2::V3Device::open(p.userId, p.backend);
        return Json{{"userId", p.userId},
                    {"protection", static_cast<int>(vault->protection())},
                    {"protectionText", p.protection},
                    {"hasDevice", p.device != nullptr}};
    }
    if (name == "config") return p.backend.server->config();
    if (name == "status") {
        Json out = {{"hasDevice", p.device != nullptr}};
        const Json view = p.backend.server->getDeviceList(p.userId);
        out["version"] = view.at("deviceList").is_null() ? Json(0) : view.at("deviceList").at("version");
        if (p.device) {
            out["deviceId"] = p.device->deviceId();
            const Json own = p.device->op("deviceList");
            out["linked"] = own.value("ok", false);
            out["trusted"] = own.value("ok", false) && own.value("trusted", false);
            if (own.value("ok", false) && !view.at("deviceList").is_null()) {
                const Json checked = p.device->op("verifyDeviceList", Json{{"deviceList", view.at("deviceList")}});
                out["listedHere"] = checked.value("ok", false) && checked.value("thisDeviceListed", true);
            }
        }
        out["registered"] = p.backend.server->listDevices();
        out["events"] = Json::array();
        for (const Json& e : p.backend.server->events()) out["events"].push_back(e.at("kind"));
        return out;
    }
    if (name == "setup") {
        p.device = e2::setUpFirstDevice(p.userId, redvault::crypto::base64Decode(p.legacyKey), p.backend);
        return Json{{"deviceId", p.device->deviceId()}};
    }
    if (name == "joinBegin") {
        p.device = e2::startNewDevice(p.userId, p.backend);
        p.joining = e2::NewDevicePairing::begin(*p.device);
        return Json{{"deviceId", p.device->deviceId()}, {"enrollmentId", p.joining->enrollmentId()}};
    }
    if (name == "joinCodes") return Json{{"codes", codesJson(p.joining->codes())}};
    if (name == "joinConfirm") {
        const Json list = p.joining->confirm();
        return Json{{"version", list.at("version")}, {"devices", list.at("devices").size()}};
    }
    if (name == "joinCancel") {
        p.joining->cancel();
        p.device->forget();
        p.device.reset();
        p.joining.reset();
        return Json::object();
    }
    if (name == "enrollments") return Json{{"items", p.backend.server->listEnrollments()}};
    if (name == "answer") {
        auto started = e2::TrustedPairing::begin(*p.device, cmd.at("enrollmentId").get<std::string>());
        p.answering = std::move(started.pairing);
        return Json{{"codes", codesJson(started.codes)}};
    }
    if (name == "approve") {
        const Json list = p.answering->confirm();
        return Json{{"version", list.at("version")}, {"devices", list.at("devices").size()}};
    }
    if (name == "reject") {
        p.answering->reject();
        return Json::object();
    }
    if (name == "revoke") {
        e2::revokeDevice(*p.device, cmd.at("deviceId").get<std::string>());
        return Json::object();
    }
    if (name == "recoveryCreate") return Json{{"key", e2::createRecoveryBackup(*p.device)}};
    if (name == "restore") {
        p.device = e2::restoreWithRecoveryKey(p.userId, cmd.at("key").get<std::string>(), p.backend);
        return Json{{"deviceId", p.device->deviceId()}};
    }
    if (name == "send") {
        const int delivered = e2::sendText(*p.device, cmd.at("conversationId").get<std::string>(), cmd.at("contactUserId").get<std::string>(),
                                           cmd.at("text").get<std::string>());
        return Json{{"delivered", delivered}};
    }
    if (name == "receive") {
        Json out = Json::array();
        for (const e2::Received& r : e2::receive(*p.device)) {
            out.push_back({{"sender", r.senderUserId}, {"senderDevice", r.senderDeviceId}, {"kind", r.kind},
                           {"text", r.text ? Json(*r.text) : Json(nullptr)}, {"error", r.error}});
        }
        return Json{{"items", out}};
    }
    if (name == "safety") {
        // What the Verify screen shows: the number, the code for the other camera, and whether it was verified.
        const auto safety = e2::deviceKeySafety(*p.device, cmd.at("contactUserId").get<std::string>(), cmd.value("conversationId", std::string()));
        if (!safety) return Json{{"groups", nullptr}};
        return Json{{"groups", safety->groups}, {"qrText", safety->qrText}, {"accountKey", safety->accountKey},
                    {"verified", safety->verified}, {"keyChanged", safety->keyChanged}};
    }
    if (name == "safetyScan") {
        // The other person's code as a camera would read it.
        return Json{{"verified", e2::verifyContactQr(*p.device, cmd.at("contactUserId").get<std::string>(), cmd.at("accountKey").get<std::string>(),
                                                     cmd.at("qrText").get<std::string>())}};
    }
    if (name == "safetyMark") {
        e2::setContactVerified(*p.device, cmd.at("contactUserId").get<std::string>(), cmd.at("accountKey").get<std::string>(),
                               cmd.at("verified").get<bool>());
        return Json::object();
    }
#ifdef REDVAULT_HAS_E2E_SIGNAL
    // rv-signal-v1 (docs/signal-integration/): what DeviceKeysSession::refresh does in the app.
    if (name == "signal") return Json{{"outcome", e2::maintainSignal(*p.device, p.backend.server->config())}, {"engine", e2::engineHasSignal()}};
#endif
    if (name == "chatSend") {
        if (!p.history) p.history = std::make_unique<e2::ChatHistory>(p.dataDir / "chat-history", p.device->deriveKey("redvault-v3-chat-history"));
        const std::string conversation = cmd.at("conversationId").get<std::string>();
        const e2::SendOutcome sent = cmd.value("group", false)
                                         ? e2::sendGroupChat(*p.device, *p.history, conversation, cmd.at("text").get<std::string>())
                                         : e2::sendChat(*p.device, *p.history, conversation, cmd.at("contactUserId").get<std::string>(),
                                                        cmd.at("text").get<std::string>());
        return Json{{"id", sent.id}, {"delivered", sent.delivered}};
    }
    if (name == "chatReceive") {
        if (!p.history) p.history = std::make_unique<e2::ChatHistory>(p.dataDir / "chat-history", p.device->deriveKey("redvault-v3-chat-history"));
        Json out = Json::array();
        for (const e2::ChatRecord& r : e2::receiveChat(*p.device, *p.history)) {
            out.push_back({{"id", r.id}, {"from", r.from}, {"text", r.text}, {"direction", r.direction}, {"conversationId", r.conversationId}});
        }
        return Json{{"items", out}, {"pinned", p.history->pinned(cmd.at("conversationId").get<std::string>())}};
    }
    if (name == "liveWait") {
        // The live channel as the app reads it: returns the event types seen until a v3-mailbox one arrives (or ~25 s).
        Json types = Json::array();
        std::optional<std::int64_t> cursor;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(25);
        bool seen = false;
        while (!seen && std::chrono::steady_clock::now() < until) {
            const auto batch = redvault::api::live::events(*p.api, cursor);
            cursor = batch.cursor;
            for (const auto& event : batch.events) {
                types.push_back(event.type);
                if (event.type == "v3-mailbox") seen = true;
            }
            if (cmd.value("once", false)) break;
        }
        return Json{{"types", types}, {"seen", seen}};
    }
    if (name == "forget") {
        p.device->forget();
        p.device.reset();
        return Json::object();
    }
    if (name == "reopen") {
        // As after an app restart: the device comes back from disk and the key vault alone.
        p.device = e2::V3Device::open(p.userId, p.backend);
        return Json{{"hasDevice", p.device != nullptr}, {"deviceId", p.device ? Json(p.device->deviceId()) : Json(nullptr)}};
    }
    throw std::runtime_error("unknown command " + name);
}

}  // namespace

int main() {
    std::ios::sync_with_stdio(false);
    Probe probe;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        Json answer;
        try {
            answer = handle(probe, Json::parse(line));
            answer["ok"] = true;
        } catch (const e2::EngineError& error) {
            answer = Json{{"ok", false}, {"error", error.code()}, {"message", error.what()}};
        } catch (const redvault::api::ApiError& error) {
            answer = Json{{"ok", false}, {"error", error.code()}, {"status", error.status()}, {"message", error.what()}};
        } catch (const std::exception& error) {
            answer = Json{{"ok", false}, {"error", "exception"}, {"message", error.what()}};
        }
        std::cout << answer.dump() << '\n' << std::flush;
    }
    return 0;
}

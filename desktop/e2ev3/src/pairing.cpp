// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/e2ev3/pairing.hpp"

#include <chrono>
#include <thread>

namespace redvault::e2ev3 {

namespace {

using Clock = std::chrono::steady_clock;

/// A stateless engine call (the SAS exchange keeps its own handle); an error answer throws.
Json stateless(const Json& request) {
    return requireOk(runEngine(request), request.value("op", std::string("engine call")));
}

void post(V3Device& from, const std::string& enrollmentId, const Json& message) {
    from.server().postEnrollmentMessage(enrollmentId, from.deviceId(), message.dump());
}

void sleepOrCancel(const WaitOptions& wait) {
    if (wait.cancelled && wait.cancelled()) throw EngineError("cancelled", "Pairing was cancelled.");
    std::this_thread::sleep_for(std::chrono::milliseconds(wait.intervalMs));
}

/// The first message of `type` the other device sent, polling until it comes.
Json waitFor(V3Device& self, const std::string& enrollmentId, const std::string& type, const WaitOptions& wait) {
    const auto until = Clock::now() + std::chrono::milliseconds(wait.timeoutMs);
    for (;;) {
        if (wait.cancelled && wait.cancelled()) throw EngineError("cancelled", "Pairing was cancelled.");
        const Json messages = self.server().enrollmentMessages(enrollmentId);
        for (const Json& m : messages) {
            if (m.value("fromDeviceId", std::string()) == self.deviceId()) continue;
            const Json body = Json::parse(m.value("body", std::string()), nullptr, false);
            if (body.is_object() && body.value("type", std::string()) == type) return body;
        }
        if (Clock::now() > until) throw EngineError("pairing_timeout", "The other device didn't answer in time.");
        sleepOrCancel(wait);
    }
}

std::vector<int> decimals(const Json& established) {
    return established.at("decimals").get<std::vector<int>>();
}

}  // namespace

// --- On the new computer ---

std::unique_ptr<NewDevicePairing> NewDevicePairing::begin(V3Device& device) {
    const Json enrollment = device.server().createEnrollment(device.deviceId());
    const std::string id = enrollment.at("id").get<std::string>();
    const Json started = stateless(Json{{"op", "sasStart"}, {"pairingId", id}, {"role", "new"}});
    post(device, id, Json{{"type", "commit"}, {"commitment", started.at("commitment")}, {"identity", device.identity()}});
    return std::unique_ptr<NewDevicePairing>(new NewDevicePairing(device, id, started.at("sasId").get<std::string>()));
}

std::vector<int> NewDevicePairing::codes(const WaitOptions& wait) {
    const Json key = waitFor(device_, enrollmentId_, "key", wait);
    const Json revealed = stateless(Json{{"op", "sasReveal"}, {"sasId", sasId_}});
    post(device_, enrollmentId_, Json{{"type", "reveal"}, {"publicKey", revealed.at("publicKey")}});
    const Json established = stateless(Json{{"op", "sasEstablish"},
                                            {"sasId", sasId_},
                                            {"theirPublicKey", key.at("publicKey")},
                                            {"newDevice", device_.identity()},
                                            {"trustedDevice", key.at("identity")}});
    return decimals(established);
}

Json NewDevicePairing::confirm(const WaitOptions& wait) {
    const Json ours = device_.must("sasMac", Json{{"sasId", sasId_}});
    post(device_, enrollmentId_, Json{{"type", "mac"}, {"mac", ours.at("mac")}});
    const Json theirs = waitFor(device_, enrollmentId_, "mac", wait);
    device_.must("sasConfirm", Json{{"sasId", sasId_}, {"mac", theirs.at("mac")}});
    const Json offer = device_.must("linkOffer");
    post(device_, enrollmentId_, Json{{"type", "offer"}, {"qr", offer.at("qrText")}});
    const auto until = Clock::now() + std::chrono::milliseconds(wait.timeoutMs);
    for (;;) {
        bool linked = false;
        for (const Received& r : receive(device_)) {
            if (r.kind == "link" && r.error.empty()) linked = true;
        }
        if (linked) break;
        if (Clock::now() > until) throw EngineError("pairing_timeout", "The trusted device didn't link this one in time.");
        sleepOrCancel(wait);
    }
    device_.publishKeys(10);
    try {
        device_.server().deleteEnrollment(enrollmentId_);
    } catch (const std::exception&) {
    }
    return device_.must("deviceList").at("deviceList");
}

void NewDevicePairing::cancel() {
    try {
        device_.server().deleteEnrollment(enrollmentId_);
    } catch (const std::exception&) {
    }
    device_.op("cancelLink");
}

// --- On a trusted device ---

TrustedPairing::Started TrustedPairing::begin(V3Device& device, const std::string& enrollmentId, const WaitOptions& wait) {
    const Json commit = waitFor(device, enrollmentId, "commit", wait);
    const Json started = stateless(Json{{"op", "sasStart"}, {"pairingId", enrollmentId}, {"role", "trusted"}});
    const Json identity = device.identity();
    post(device, enrollmentId, Json{{"type", "key"}, {"publicKey", started.at("publicKey")}, {"identity", identity}});
    const Json reveal = waitFor(device, enrollmentId, "reveal", wait);
    const Json established = stateless(Json{{"op", "sasEstablish"},
                                            {"sasId", started.at("sasId")},
                                            {"theirPublicKey", reveal.at("publicKey")},
                                            {"theirCommitment", commit.at("commitment")},
                                            {"newDevice", commit.at("identity")},
                                            {"trustedDevice", identity}});
    return Started{std::unique_ptr<TrustedPairing>(
                       new TrustedPairing(device, enrollmentId, started.at("sasId").get<std::string>(), commit.at("identity"))),
                   decimals(established)};
}

Json TrustedPairing::confirm(const WaitOptions& wait) {
    const Json ours = device_.must("sasMac", Json{{"sasId", sasId_}});
    post(device_, enrollmentId_, Json{{"type", "mac"}, {"mac", ours.at("mac")}});
    const Json theirs = waitFor(device_, enrollmentId_, "mac", wait);
    device_.must("sasConfirm", Json{{"sasId", sasId_}, {"mac", theirs.at("mac")}});
    const Json offer = waitFor(device_, enrollmentId_, "offer", wait);
    const Json approved = publishList(device_, [&] {
        return device_.must("approveDevice", Json{{"qr", offer.at("qr")}, {"verifiedBy", "sas"}});
    });
    device_.server().sendMessages(
        device_.deviceId(), "",
        Json::array({Json{{"userId", newDevice_.at("userId")}, {"deviceId", newDevice_.at("deviceId")}, {"body", approved.at("message").dump()}}}));
    return approved.at("deviceList");
}

void TrustedPairing::reject() {
    try {
        device_.server().deleteEnrollment(enrollmentId_);
    } catch (const std::exception&) {
    }
}

}  // namespace redvault::e2ev3

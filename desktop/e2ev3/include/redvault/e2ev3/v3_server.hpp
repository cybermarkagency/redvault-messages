// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <string>

#include "redvault/api/api_client.hpp"
#include "redvault/e2ev3/engine.hpp"

// The server's protocol-3 routes (server/src/routes/e2eV3Routes.ts), mirroring
// ui/src/lib/api.ts. Public keys, signed lists and ciphertext only; the server
// is off for all of it unless E2E_V3_ENABLED is set. Everything here is plain
// JSON in and out, as the engine produces and consumes it, and every failure
// is an api::ApiError with the server's code ("stale_device_list", ...).
// An interface so the tests can stand in a small in-memory server.

namespace redvault::e2ev3 {

class V3Server {
public:
    virtual ~V3Server() = default;
    /// {enabled, resetDelayHours}
    virtual Json config() = 0;
    /// `upload` is the engine's {identity, oneTimeKeys, fallbackKey}.
    virtual Json registerDevice(const Json& upload) = 0;
    virtual Json listDevices() = 0;
    virtual void removeDevice(const std::string& deviceId) = 0;
    /// {deviceList, pendingReset}
    virtual Json getDeviceList(const std::string& userId, const std::string& conversationId = {}) = 0;
    virtual Json putDeviceList(const Json& deviceList, const Json& rotationProof) = 0;
    virtual void cancelReset(const std::string& signature) = 0;
    virtual Json claimKeys(const std::string& conversationId, const Json& devices) = 0;
    virtual void sendMessages(const std::string& senderDeviceId, const std::string& conversationId, const Json& messages) = 0;
    /// {members: [{userId, deviceList, pendingReset}]}: the signed list of every active participant (the caller verifies each).
    virtual Json conversationDeviceLists(const std::string& conversationId) = 0;
    /// One body fanned out to many devices; `recipients` is [{userId, deviceId}].
    virtual void sendGroupMessage(const std::string& senderDeviceId, const std::string& conversationId, const std::string& body,
                                  const Json& recipients) = 0;
    virtual Json inbox(const std::string& deviceId) = 0;
    virtual void ack(const std::string& deviceId, const Json& ids) = 0;
    virtual Json createEnrollment(const std::string& newDeviceId) = 0;
    virtual Json listEnrollments() = 0;
    virtual Json postEnrollmentMessage(const std::string& id, const std::string& fromDeviceId, const std::string& body) = 0;
    virtual Json enrollmentMessages(const std::string& id, int after = 0) = 0;
    virtual void deleteEnrollment(const std::string& id) = 0;
    virtual void putRecoveryBackup(const std::string& backup, const std::string& signature) = 0;
    virtual Json getRecoveryBackup() = 0;
    virtual Json events(int after = 0) = 0;

    // --- rv-signal-v1 prekey directory (E2E_SIGNAL_ENABLED; docs/signal-integration/) ---
    // Not pure: a server or test double without Signal answers like a server with it switched off.

    /// Stores the engine's signalPrekeys upload for this account's device. Answers the key counts.
    virtual Json signalUpload(const std::string& deviceId, const Json& upload);
    /// {enabled, preKeys, kyberPreKeys, signedPreKeyId, lastResortKyberPreKeyId}
    virtual Json signalStatus(const std::string& deviceId);
    /// One {userId, deviceId, certificate, bundle} per device; one-time keys are used up.
    virtual Json signalClaim(const std::string& conversationId, const Json& devices);
    /// M6: a conversation's signed rosters after `afterEpoch`, oldest first (0: the whole history).
    virtual Json rosters(const std::string& conversationId, int afterEpoch = 0);
    /// M6: stores the next roster; ApiError "stale_roster" when another member's came first.
    virtual Json postRoster(const std::string& conversationId, const Json& roster);
};

/// The real thing, over the app's signed-in ApiClient.
class HttpV3Server final : public V3Server {
public:
    explicit HttpV3Server(std::shared_ptr<api::ApiClient> api) : api_(std::move(api)) {}

    Json config() override;
    Json registerDevice(const Json& upload) override;
    Json listDevices() override;
    void removeDevice(const std::string& deviceId) override;
    Json getDeviceList(const std::string& userId, const std::string& conversationId = {}) override;
    Json putDeviceList(const Json& deviceList, const Json& rotationProof) override;
    void cancelReset(const std::string& signature) override;
    Json claimKeys(const std::string& conversationId, const Json& devices) override;
    void sendMessages(const std::string& senderDeviceId, const std::string& conversationId, const Json& messages) override;
    Json conversationDeviceLists(const std::string& conversationId) override;
    void sendGroupMessage(const std::string& senderDeviceId, const std::string& conversationId, const std::string& body,
                          const Json& recipients) override;
    Json inbox(const std::string& deviceId) override;
    void ack(const std::string& deviceId, const Json& ids) override;
    Json createEnrollment(const std::string& newDeviceId) override;
    Json listEnrollments() override;
    Json postEnrollmentMessage(const std::string& id, const std::string& fromDeviceId, const std::string& body) override;
    Json enrollmentMessages(const std::string& id, int after = 0) override;
    void deleteEnrollment(const std::string& id) override;
    void putRecoveryBackup(const std::string& backup, const std::string& signature) override;
    Json getRecoveryBackup() override;
    Json events(int after = 0) override;
    Json signalUpload(const std::string& deviceId, const Json& upload) override;
    Json signalStatus(const std::string& deviceId) override;
    Json signalClaim(const std::string& conversationId, const Json& devices) override;
    Json rosters(const std::string& conversationId, int afterEpoch = 0) override;
    Json postRoster(const std::string& conversationId, const Json& roster) override;

private:
    std::shared_ptr<api::ApiClient> api_;
};

}  // namespace redvault::e2ev3

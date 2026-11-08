// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "redvault/e2ev3/device.hpp"

// Pairing a new computer with a trusted device of the same account by comparing
// codes (crypto-core/src/pairing.rs: vodozemac's SAS with a key commitment), for
// when neither can scan the other's QR code. A port of ui/src/lib/e2eV3/pairing.ts.
// The server only relays the messages; a relay that swaps keys makes the two
// screens show different numbers, and the person stops.
//
// All calls block while they wait for the other device: run them off the UI thread.

namespace redvault::e2ev3 {

struct WaitOptions {
    int intervalMs = 1000;
    int timeoutMs = 10 * 60 * 1000;
    /// Polled between requests; returning true abandons the wait with EngineError("cancelled").
    std::function<bool()> cancelled;
};

/// On the new computer.
class NewDevicePairing {
public:
    /// Asks the account's trusted devices to pair; they're told and show the request.
    static std::unique_ptr<NewDevicePairing> begin(V3Device& device);

    /// Waits for a trusted device to answer; returns the numbers to compare with its screen.
    std::vector<int> codes(const WaitOptions& wait = {});
    /// The person said the numbers match. Returns the account's list once the trusted device has linked this one.
    Json confirm(const WaitOptions& wait = {});
    /// The numbers didn't match, or the person gave up.
    void cancel();

    const std::string& enrollmentId() const { return enrollmentId_; }

private:
    NewDevicePairing(V3Device& device, std::string enrollmentId, std::string sasId)
        : device_(device), enrollmentId_(std::move(enrollmentId)), sasId_(std::move(sasId)) {}
    V3Device& device_;
    std::string enrollmentId_;
    std::string sasId_;
};

/// On a trusted device, answering a pairing request.
class TrustedPairing {
public:
    struct Started {
        std::unique_ptr<TrustedPairing> pairing;
        std::vector<int> codes;
    };
    /// Answers the request; returns the pairing and the numbers to compare with the new device's screen.
    static Started begin(V3Device& device, const std::string& enrollmentId, const WaitOptions& wait = {});

    /// The person said the numbers match: approves exactly the device compared with, signs the new list and sends the link.
    Json confirm(const WaitOptions& wait = {});
    /// The numbers didn't match: nothing is approved.
    void reject();

    const Json& newDevice() const { return newDevice_; }
    const std::string& enrollmentId() const { return enrollmentId_; }

private:
    TrustedPairing(V3Device& device, std::string enrollmentId, std::string sasId, Json newDevice)
        : device_(device), enrollmentId_(std::move(enrollmentId)), sasId_(std::move(sasId)), newDevice_(std::move(newDevice)) {}
    V3Device& device_;
    std::string enrollmentId_;
    std::string sasId_;
    Json newDevice_;
};

}  // namespace redvault::e2ev3

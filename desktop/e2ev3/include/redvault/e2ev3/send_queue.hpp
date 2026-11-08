// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "redvault/crypto/bytes.hpp"
#include "redvault/e2ev3/v3_server.hpp"

// Device-key sends that wait for Tor (docs/tor-by-default.md, rule 5). Nothing is
// ever sent around the Tor transport and nothing is lost while Tor is down:
// - a send made while Tor isn't connected stays in the encrypted chat history as
//   "queued" (queueChat in chat.hpp) and is encrypted once, later, in order;
// - a send whose engine work was done but whose request couldn't leave (the gate
//   refused it: "tor_unavailable", so nothing reached the server) keeps exactly
//   that request, the per-device ciphertext, in a SendQueue. The engine saved its
//   state before the request was made, so the ciphertext is never made again: it
//   goes out as it is, in order, once Tor is back (sendQueuedChat).

namespace redvault::e2ev3 {

/// One request the engine already encrypted: what goes to POST e2e/v3/messages or e2e/v3/group-messages, as it is.
struct QueuedSend {
    std::string id;
    std::string kind;  // "messages" | "group"
    std::string senderDeviceId;
    std::string conversationId;
    Json messages = Json::array();  // "messages": [{userId, deviceId, body}]
    std::string body;  // "group": the one ciphertext
    Json recipients = Json::array();  // "group": [{userId, deviceId}]
    /// The chat record this request belongs to ("" for none).
    std::string recordId;
    /// True when the request carries that record's own message, not a key share sent ahead of it.
    bool carriesRecord = false;
};

/// The queued requests, oldest first, in one file sealed with XChaCha20-Poly1305 and rewritten on each change.
class SendQueue {
public:
    SendQueue(std::filesystem::path file, crypto::Bytes key);
    void push(QueuedSend item);
    void remove(const std::string& id);
    std::vector<QueuedSend> items() const;
    bool empty() const;

private:
    void load();
    void save() const;
    std::filesystem::path file_;
    crypto::Bytes key_;
    mutable std::mutex mutex_;
    std::vector<QueuedSend> items_;
};

/// Thrown by QueueingV3Server once it has queued a request: nothing went out; the ciphertext waits in the queue.
class SendQueued : public std::runtime_error {
public:
    SendQueued() : std::runtime_error("Tor isn't connected: the message waits, encrypted, until it is") {}
};

/// Marks the chat send running on this thread (chat.cpp, around one delivery). Only sends inside one are ever queued.
class SendScope {
public:
    /// `carrierKind`: which request carries the record's own message ("messages" one-to-one, "group" in a group).
    SendScope(std::string recordId, std::string carrierKind);
    ~SendScope();
    SendScope(const SendScope&) = delete;
    SendScope& operator=(const SendScope&) = delete;
    /// The innermost scope on this thread, or null.
    static const SendScope* current();

private:
    const SendScope* previous_;

public:
    const std::string recordId;
    const std::string carrierKind;
};

/**
 * The server, with the chat sends queued instead of lost while Tor is down. Every
 * call goes to `inner` (the app's HttpV3Server, over the Tor transport) unchanged,
 * except sendMessages and sendGroupMessage inside a SendScope: those are queued,
 * then SendQueued is thrown, when the inner call is refused with "tor_unavailable"
 * (the gate opened no socket) or when earlier requests are still queued (order).
 * Without an attached queue it's a plain pass-through.
 */
class QueueingV3Server final : public V3Server {
public:
    explicit QueueingV3Server(std::shared_ptr<V3Server> inner) : inner_(std::move(inner)) {}

    /// The queue of the device in use (sendQueuedChat drains it). Null detaches.
    void attach(std::shared_ptr<SendQueue> queue);
    std::shared_ptr<SendQueue> queue() const;

    struct Flushed {
        /// Records whose own message went out: (conversationId, recordId).
        std::vector<std::pair<std::string, std::string>> sent;
        /// Records whose request the server refused for good (not Tor, not busy): they failed.
        std::vector<std::pair<std::string, std::string>> refused;
        /// Tor or the server isn't reachable yet: this request and the ones after it still wait.
        bool waiting = false;
    };
    /// Sends what's queued, oldest first, exactly as queued.
    Flushed flush();

    Json config() override { return inner_->config(); }
    Json registerDevice(const Json& upload) override { return inner_->registerDevice(upload); }
    Json listDevices() override { return inner_->listDevices(); }
    void removeDevice(const std::string& deviceId) override { inner_->removeDevice(deviceId); }
    Json getDeviceList(const std::string& userId, const std::string& conversationId = {}) override {
        return inner_->getDeviceList(userId, conversationId);
    }
    Json putDeviceList(const Json& deviceList, const Json& rotationProof) override { return inner_->putDeviceList(deviceList, rotationProof); }
    void cancelReset(const std::string& signature) override { inner_->cancelReset(signature); }
    Json claimKeys(const std::string& conversationId, const Json& devices) override { return inner_->claimKeys(conversationId, devices); }
    void sendMessages(const std::string& senderDeviceId, const std::string& conversationId, const Json& messages) override;
    Json conversationDeviceLists(const std::string& conversationId) override { return inner_->conversationDeviceLists(conversationId); }
    void sendGroupMessage(const std::string& senderDeviceId, const std::string& conversationId, const std::string& body,
                          const Json& recipients) override;
    Json inbox(const std::string& deviceId) override { return inner_->inbox(deviceId); }
    void ack(const std::string& deviceId, const Json& ids) override { inner_->ack(deviceId, ids); }
    Json createEnrollment(const std::string& newDeviceId) override { return inner_->createEnrollment(newDeviceId); }
    Json listEnrollments() override { return inner_->listEnrollments(); }
    Json postEnrollmentMessage(const std::string& id, const std::string& fromDeviceId, const std::string& body) override {
        return inner_->postEnrollmentMessage(id, fromDeviceId, body);
    }
    Json enrollmentMessages(const std::string& id, int after = 0) override { return inner_->enrollmentMessages(id, after); }
    void deleteEnrollment(const std::string& id) override { inner_->deleteEnrollment(id); }
    void putRecoveryBackup(const std::string& backup, const std::string& signature) override { inner_->putRecoveryBackup(backup, signature); }
    Json getRecoveryBackup() override { return inner_->getRecoveryBackup(); }
    Json events(int after = 0) override { return inner_->events(after); }
    Json signalUpload(const std::string& deviceId, const Json& upload) override { return inner_->signalUpload(deviceId, upload); }
    Json signalStatus(const std::string& deviceId) override { return inner_->signalStatus(deviceId); }
    Json signalClaim(const std::string& conversationId, const Json& devices) override { return inner_->signalClaim(conversationId, devices); }
    Json rosters(const std::string& conversationId, int afterEpoch = 0) override { return inner_->rosters(conversationId, afterEpoch); }
    Json postRoster(const std::string& conversationId, const Json& roster) override { return inner_->postRoster(conversationId, roster); }

private:
    void sendOrQueue(QueuedSend item);
    void sendNow(const QueuedSend& item);
    std::shared_ptr<V3Server> inner_;
    mutable std::mutex mutex_;
    std::shared_ptr<SendQueue> queue_;
};

/// Worth waiting for rather than giving up on: Tor or the network down, or the server busy or briefly unavailable.
bool waitsForConnection(const api::ApiError& error);

}  // namespace redvault::e2ev3

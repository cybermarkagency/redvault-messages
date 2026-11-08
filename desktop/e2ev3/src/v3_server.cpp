// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/e2ev3/v3_server.hpp"

#include "redvault/api/api_error.hpp"
#include "redvault/net/http.hpp"

namespace redvault::e2ev3 {

namespace {

using net::percentEncode;

Json nullIfEmpty(const std::string& text) {
    return text.empty() ? Json(nullptr) : Json(text);
}

}  // namespace

Json HttpV3Server::config() {
    return api_->call("GET", "e2e/v3/config");
}

Json HttpV3Server::registerDevice(const Json& upload) {
    const std::string deviceId = upload.at("identity").at("deviceId").get<std::string>();
    return api_->call("PUT", "e2e/v3/devices/" + percentEncode(deviceId), Json(upload));
}

Json HttpV3Server::listDevices() {
    return api_->call("GET", "e2e/v3/devices");
}

void HttpV3Server::removeDevice(const std::string& deviceId) {
    api_->call("DELETE", "e2e/v3/devices/" + percentEncode(deviceId));
}

Json HttpV3Server::getDeviceList(const std::string& userId, const std::string& conversationId) {
    std::string path = "e2e/v3/users/" + percentEncode(userId) + "/device-list";
    if (!conversationId.empty()) path += "?conversationId=" + percentEncode(conversationId);
    return api_->call("GET", path);
}

Json HttpV3Server::putDeviceList(const Json& deviceList, const Json& rotationProof) {
    return api_->call("PUT", "e2e/v3/device-list", Json{{"deviceList", deviceList}, {"rotationProof", rotationProof}});
}

void HttpV3Server::cancelReset(const std::string& signature) {
    api_->call("POST", "e2e/v3/device-list/cancel-reset", Json{{"signature", signature}});
}

Json HttpV3Server::claimKeys(const std::string& conversationId, const Json& devices) {
    return api_->call("POST", "e2e/v3/claim", Json{{"conversationId", nullIfEmpty(conversationId)}, {"devices", devices}});
}

void HttpV3Server::sendMessages(const std::string& senderDeviceId, const std::string& conversationId, const Json& messages) {
    api_->call("POST", "e2e/v3/messages",
               Json{{"senderDeviceId", senderDeviceId}, {"conversationId", nullIfEmpty(conversationId)}, {"messages", messages}});
}

Json HttpV3Server::conversationDeviceLists(const std::string& conversationId) {
    return api_->call("GET", "e2e/v3/conversations/" + percentEncode(conversationId) + "/device-lists");
}

void HttpV3Server::sendGroupMessage(const std::string& senderDeviceId, const std::string& conversationId, const std::string& body,
                                    const Json& recipients) {
    api_->call("POST", "e2e/v3/group-messages",
               Json{{"senderDeviceId", senderDeviceId}, {"conversationId", conversationId}, {"body", body}, {"recipients", recipients}});
}

Json HttpV3Server::inbox(const std::string& deviceId) {
    return api_->call("GET", "e2e/v3/devices/" + percentEncode(deviceId) + "/messages");
}

void HttpV3Server::ack(const std::string& deviceId, const Json& ids) {
    api_->call("POST", "e2e/v3/devices/" + percentEncode(deviceId) + "/messages/ack", Json{{"ids", ids}});
}

Json HttpV3Server::createEnrollment(const std::string& newDeviceId) {
    return api_->call("POST", "e2e/v3/enrollments", Json{{"newDeviceId", newDeviceId}});
}

Json HttpV3Server::listEnrollments() {
    return api_->call("GET", "e2e/v3/enrollments");
}

Json HttpV3Server::postEnrollmentMessage(const std::string& id, const std::string& fromDeviceId, const std::string& body) {
    return api_->call("POST", "e2e/v3/enrollments/" + percentEncode(id) + "/messages", Json{{"fromDeviceId", fromDeviceId}, {"body", body}});
}

Json HttpV3Server::enrollmentMessages(const std::string& id, int after) {
    return api_->call("GET", "e2e/v3/enrollments/" + percentEncode(id) + "/messages?after=" + std::to_string(after));
}

void HttpV3Server::deleteEnrollment(const std::string& id) {
    api_->call("DELETE", "e2e/v3/enrollments/" + percentEncode(id));
}

void HttpV3Server::putRecoveryBackup(const std::string& backup, const std::string& signature) {
    api_->call("PUT", "e2e/v3/recovery-backup", Json{{"backup", backup}, {"signature", signature}});
}

Json HttpV3Server::getRecoveryBackup() {
    return api_->call("GET", "e2e/v3/recovery-backup");
}

Json HttpV3Server::events(int after) {
    return api_->call("GET", "e2e/v3/events?after=" + std::to_string(after));
}

// --- rv-signal-v1 ---

Json V3Server::signalUpload(const std::string&, const Json&) {
    throw api::ApiError(503, "e2e_signal_disabled", "Signal sessions aren't available here.");
}

Json V3Server::signalStatus(const std::string&) {
    throw api::ApiError(503, "e2e_signal_disabled", "Signal sessions aren't available here.");
}

Json V3Server::signalClaim(const std::string&, const Json&) {
    throw api::ApiError(503, "e2e_signal_disabled", "Signal sessions aren't available here.");
}

Json HttpV3Server::signalUpload(const std::string& deviceId, const Json& upload) {
    return api_->call("PUT", "e2e/v3/devices/" + percentEncode(deviceId) + "/signal", Json(upload));
}

Json HttpV3Server::signalStatus(const std::string& deviceId) {
    return api_->call("GET", "e2e/v3/devices/" + percentEncode(deviceId) + "/signal");
}

Json HttpV3Server::signalClaim(const std::string& conversationId, const Json& devices) {
    return api_->call("POST", "e2e/v3/signal/claim", Json{{"conversationId", nullIfEmpty(conversationId)}, {"devices", devices}});
}

Json V3Server::rosters(const std::string&, int) {
    throw api::ApiError(503, "e2e_signal_disabled", "Signal sessions aren't available here.");
}

Json V3Server::postRoster(const std::string&, const Json&) {
    throw api::ApiError(503, "e2e_signal_disabled", "Signal sessions aren't available here.");
}

Json HttpV3Server::rosters(const std::string& conversationId, int afterEpoch) {
    return api_->call("GET", "e2e/v3/conversations/" + percentEncode(conversationId) + "/rosters?after=" + std::to_string(afterEpoch));
}

Json HttpV3Server::postRoster(const std::string& conversationId, const Json& roster) {
    return api_->call("POST", "e2e/v3/conversations/" + percentEncode(conversationId) + "/rosters", Json{{"roster", roster}});
}

}  // namespace redvault::e2ev3

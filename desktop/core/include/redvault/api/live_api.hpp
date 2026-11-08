// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"

// The endpoints added with calls and the redesign (server/src/routes/
// callRoutes.ts, liveRoutes.ts, authRoutes.ts' sessions, transparencyRoutes.ts'
// log), mirroring ui/src/lib/api.ts. Free functions over ApiClient::call so
// the long-standing client stays as it was. All of them block.

namespace redvault::api::live {

/// A TURN relay and short-lived credentials for it.
struct IceServer {
    std::vector<std::string> urls;
    std::string username;
    std::string credential;
};

/// An offer, answer or later signal, sealed end to end (base64). The server can't read or forge it.
struct SealedSignal {
    std::string ciphertext;
    std::string nonce;
};

struct StartedCall {
    std::string callId;
    std::string calleeId;
    /// Set when joining a call link (the link decides the kind).
    std::optional<std::string> kind;
    std::vector<IceServer> iceServers;
};

/// Someone in a group call, with the pseudonym they have in the LiveKit room.
struct GroupMember {
    std::string userId;
    std::string identity;
};

/**
 * One event from the live channel. `type` is "incoming", "answered",
 * "signal", "screen", "ended" or "typing"; the other fields are set as
 * that type has them (see server/src/services/callService.ts's LiveEvent).
 * Group calls add "group-incoming", "group-roster", "group-key",
 * "group-ring-stop" and "group-ended" (server/src/services/groupCallService.ts).
 */
struct LiveEvent {
    std::string type;
    std::string callId;
    std::optional<std::string> conversationId;
    std::optional<std::string> linkId;
    std::optional<std::string> callerId;
    std::optional<std::string> kind;
    std::optional<std::string> userId;
    std::optional<std::string> reason;
    std::optional<SealedSignal> sealed;  // the offer, answer or signal (a group call's room key, for "group-key")
    std::vector<IceServer> iceServers;
    bool on = false;
    // Group calls:
    std::optional<std::string> hostId;      // group-incoming: who started it
    std::int64_t epoch = 0;                 // group-roster, group-key
    std::optional<std::string> keyholder;   // group-roster: the first person still in the call
    std::vector<GroupMember> members;       // group-roster: who is in the call now
    std::optional<std::string> from;        // group-key: the keyholder that sealed it
};

struct LiveEvents {
    std::int64_t cursor = 0;
    std::vector<LiveEvent> events;
    bool callsEnabled = false;
    /// The server has a LiveKit server for group calls (the apps hide the buttons otherwise).
    bool groupCallsEnabled = false;
};

// --- The live channel ---

/// Long poll: returns new events, or none after ~25 s. Without `after`, returns at once with any call ringing now.
LiveEvents events(ApiClient& api, std::optional<std::int64_t> after);
/// "I'm typing in this chat" (send only while typing indicators are on).
void sendTyping(ApiClient& api, const std::string& conversationId);
/// Which contacts are online (empty unless you share your own status).
std::vector<std::string> presence(ApiClient& api);

// --- Calls ---

StartedCall startCall(ApiClient& api, const std::string& conversationId, const std::string& kind);
void sendOffer(ApiClient& api, const std::string& callId, const SealedSignal& offer);
void sendAnswer(ApiClient& api, const std::string& callId, const SealedSignal& answer);
void sendSignal(ApiClient& api, const std::string& callId, const SealedSignal& signal);
void declineCall(ApiClient& api, const std::string& callId);
void hangUp(ApiClient& api, const std::string& callId);
/// Screen sharing is Premium: the server checks, then tells the other side.
void setScreenShare(ApiClient& api, const std::string& callId, bool on);
/// TURN credentials on their own, for Settings' relay check.
std::vector<IceServer> relays(ApiClient& api);

// --- Call links ---

struct CallLinkDto {
    std::string id;
    std::string kind;
    std::string secretSealed;
    std::string nameCiphertext;
    std::string nameNonce;
    std::string createdOn;
};

struct CallLinkInfo {
    std::string id;
    std::string kind;
    std::string ownerId;
    std::string nameCiphertext;
    std::string nameNonce;
};

std::vector<CallLinkDto> listCallLinks(ApiClient& api);
CallLinkDto createCallLink(ApiClient& api, const std::string& kind, const std::string& secretSealed,
                           const std::string& nameCiphertext, const std::string& nameNonce);
void deleteCallLink(ApiClient& api, const std::string& id);
CallLinkInfo callLinkInfo(ApiClient& api, const std::string& id);
StartedCall joinCallLink(ApiClient& api, const std::string& id);

// --- Linked devices ---

struct SessionDto {
    std::string id;
    bool current = false;
    bool awaitingTwoFactor = false;
    std::string expiresOn;
    std::optional<SealedSignal> label;  // the device's name, encrypted by your own devices
};

std::vector<SessionDto> listSessions(ApiClient& api);
void setSessionLabel(ApiClient& api, const SealedSignal& label);
void signOutSession(ApiClient& api, const std::string& id);

// --- Verifiable transparency log (public) ---

struct LogCheckpoint {
    std::int64_t treeSize = 0;
    std::string rootHash;  // hex
    std::string signedAt;
    std::string signature;  // base64
    std::string publicKey;  // base64
};

struct LogEntry {
    std::int64_t index = 0;
    std::string commitment;  // hex
    int sizeKb = 0;
    std::int64_t checkpointSize = 0;
    std::string loggedAt;
};

struct InclusionProof {
    std::int64_t index = 0;
    std::int64_t treeSize = 0;
    std::string leafHash;
    std::vector<std::string> path;
};

/// Null when the log has no entries yet. ApiError "log_disabled" (503) when the server has no log.
std::optional<LogCheckpoint> latestCheckpoint(ApiClient& api);
std::vector<LogEntry> logEntries(ApiClient& api, std::optional<std::int64_t> before, int limit, const std::string& search = {});
InclusionProof inclusionProof(ApiClient& api, std::int64_t index, std::int64_t treeSize);
std::vector<std::string> consistencyProof(ApiClient& api, std::int64_t from, std::int64_t to);

/// Percent-encodes one query-string value.
std::string urlEncode(const std::string& value);

}  // namespace redvault::api::live

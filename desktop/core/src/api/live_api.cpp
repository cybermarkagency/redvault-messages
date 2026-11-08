// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/api/live_api.hpp"

#include <nlohmann/json.hpp>

#include "redvault/api/api_error.hpp"

#include "redvault/util/text.hpp"

namespace redvault::api::live {

namespace {

template <typename T>
T field(const Json& j, const char* key) {
    return j.at(key).get<T>();
}

template <typename T>
std::optional<T> optField(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    return it->get<T>();
}

/// Parses a 2xx body, turning a wrong shape into "malformed_response" like ApiClient does.
template <typename F>
auto parse(const Json& j, F&& f) -> decltype(f(j)) {
    try {
        return f(j);
    } catch (const Json::exception& e) {
        throw ApiError(0, "malformed_response", std::string("Unexpected response: ") + e.what());
    }
}

IceServer iceServer(const Json& j) {
    IceServer s;
    s.urls = field<std::vector<std::string>>(j, "urls");
    s.username = field<std::string>(j, "username");
    s.credential = field<std::string>(j, "credential");
    return s;
}

std::vector<IceServer> iceServers(const Json& j, const char* key) {
    std::vector<IceServer> out;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& s : *it) out.push_back(iceServer(s));
    return out;
}

SealedSignal sealed(const Json& j) {
    return SealedSignal{field<std::string>(j, "ciphertext"), field<std::string>(j, "nonce")};
}

Json toJson(const SealedSignal& s) {
    return Json{{"ciphertext", s.ciphertext}, {"nonce", s.nonce}};
}

StartedCall startedCall(const Json& j) {
    StartedCall c;
    c.callId = field<std::string>(j, "callId");
    c.calleeId = field<std::string>(j, "calleeId");
    c.kind = optField<std::string>(j, "kind");
    c.iceServers = iceServers(j, "iceServers");
    return c;
}

LiveEvent liveEvent(const Json& j) {
    LiveEvent e;
    e.type = field<std::string>(j, "type");
    e.callId = optField<std::string>(j, "callId").value_or("");
    e.conversationId = optField<std::string>(j, "conversationId");
    e.linkId = optField<std::string>(j, "linkId");
    e.callerId = optField<std::string>(j, "callerId");
    e.kind = optField<std::string>(j, "kind");
    e.userId = optField<std::string>(j, "userId");
    if (e.userId) e.userId = util::toLowerAscii(*e.userId);
    e.reason = optField<std::string>(j, "reason");
    for (const char* key : {"offer", "answer", "signal", "key"}) {
        const auto it = j.find(key);
        if (it != j.end() && it->is_object()) e.sealed = sealed(*it);
    }
    e.iceServers = iceServers(j, "iceServers");
    e.on = optField<bool>(j, "on").value_or(false);
    e.hostId = optField<std::string>(j, "hostId");
    if (e.hostId) e.hostId = util::toLowerAscii(*e.hostId);
    e.epoch = optField<std::int64_t>(j, "epoch").value_or(0);
    e.keyholder = optField<std::string>(j, "keyholder");
    if (e.keyholder) e.keyholder = util::toLowerAscii(*e.keyholder);
    e.from = optField<std::string>(j, "from");
    if (e.from) e.from = util::toLowerAscii(*e.from);
    const auto members = j.find("members");
    if (members != j.end() && members->is_array()) {
        for (const auto& m : *members) e.members.push_back(GroupMember{util::toLowerAscii(field<std::string>(m, "userId")), field<std::string>(m, "identity")});
    }
    return e;
}

std::string callPath(const std::string& callId, const char* action) {
    return "calls/" + urlEncode(callId) + "/" + action;
}

}  // namespace

std::string urlEncode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

LiveEvents events(ApiClient& api, std::optional<std::int64_t> after) {
    const Json j = api.call("GET", after ? "live/events?after=" + std::to_string(*after) : std::string("live/events"));
    return parse(j, [](const Json& body) {
        LiveEvents out;
        out.cursor = field<std::int64_t>(body, "cursor");
        out.callsEnabled = optField<bool>(body, "callsEnabled").value_or(true);
        // A server from before group calls doesn't say: no.
        out.groupCallsEnabled = optField<bool>(body, "groupCallsEnabled").value_or(false);
        for (const auto& e : body.at("events")) {
            // An event type this build doesn't know is skipped, not fatal.
            try {
                out.events.push_back(liveEvent(e));
            } catch (const Json::exception&) {
            }
        }
        return out;
    });
}

void sendTyping(ApiClient& api, const std::string& conversationId) {
    api.call("POST", "live/typing", Json{{"conversationId", conversationId}});
}

std::vector<std::string> presence(ApiClient& api) {
    const Json j = api.call("GET", "live/presence");
    return parse(j, [](const Json& body) { return field<std::vector<std::string>>(body, "online"); });
}

StartedCall startCall(ApiClient& api, const std::string& conversationId, const std::string& kind) {
    const Json j = api.call("POST", "calls", Json{{"conversationId", conversationId}, {"kind", kind}});
    return parse(j, startedCall);
}

void sendOffer(ApiClient& api, const std::string& callId, const SealedSignal& offer) {
    api.call("POST", callPath(callId, "offer"), toJson(offer));
}

void sendAnswer(ApiClient& api, const std::string& callId, const SealedSignal& answer) {
    api.call("POST", callPath(callId, "answer"), toJson(answer));
}

void sendSignal(ApiClient& api, const std::string& callId, const SealedSignal& signal) {
    api.call("POST", callPath(callId, "signal"), toJson(signal));
}

void declineCall(ApiClient& api, const std::string& callId) {
    api.call("POST", callPath(callId, "decline"));
}

void hangUp(ApiClient& api, const std::string& callId) {
    api.call("POST", callPath(callId, "hangup"));
}

void setScreenShare(ApiClient& api, const std::string& callId, bool on) {
    api.call("POST", callPath(callId, "screen"), Json{{"on", on}});
}

std::vector<IceServer> relays(ApiClient& api) {
    const Json j = api.call("GET", "calls/relays");
    return parse(j, [](const Json& body) { return iceServers(body, "iceServers"); });
}

std::vector<CallLinkDto> listCallLinks(ApiClient& api) {
    const Json j = api.call("GET", "call-links");
    return parse(j, [](const Json& body) {
        std::vector<CallLinkDto> out;
        for (const auto& l : body) {
            out.push_back(CallLinkDto{field<std::string>(l, "id"), field<std::string>(l, "kind"), field<std::string>(l, "secretSealed"),
                                      field<std::string>(l, "nameCiphertext"), field<std::string>(l, "nameNonce"),
                                      optField<std::string>(l, "createdOn").value_or("")});
        }
        return out;
    });
}

CallLinkDto createCallLink(ApiClient& api, const std::string& kind, const std::string& secretSealed,
                           const std::string& nameCiphertext, const std::string& nameNonce) {
    const Json j = api.call("POST", "call-links",
                            Json{{"kind", kind}, {"secretSealed", secretSealed}, {"nameCiphertext", nameCiphertext}, {"nameNonce", nameNonce}});
    return parse(j, [](const Json& l) {
        return CallLinkDto{field<std::string>(l, "id"), field<std::string>(l, "kind"), field<std::string>(l, "secretSealed"),
                           field<std::string>(l, "nameCiphertext"), field<std::string>(l, "nameNonce"),
                           optField<std::string>(l, "createdOn").value_or("")};
    });
}

void deleteCallLink(ApiClient& api, const std::string& id) {
    api.call("DELETE", "call-links/" + urlEncode(id));
}

CallLinkInfo callLinkInfo(ApiClient& api, const std::string& id) {
    const Json j = api.call("GET", "call-links/" + urlEncode(id));
    return parse(j, [](const Json& l) {
        return CallLinkInfo{field<std::string>(l, "id"), field<std::string>(l, "kind"), field<std::string>(l, "ownerId"),
                            field<std::string>(l, "nameCiphertext"), field<std::string>(l, "nameNonce")};
    });
}

StartedCall joinCallLink(ApiClient& api, const std::string& id) {
    const Json j = api.call("POST", "call-links/" + urlEncode(id) + "/join");
    return parse(j, startedCall);
}

std::vector<SessionDto> listSessions(ApiClient& api) {
    const Json j = api.call("GET", "auth/sessions");
    return parse(j, [](const Json& body) {
        std::vector<SessionDto> out;
        for (const auto& s : body) {
            SessionDto d;
            d.id = field<std::string>(s, "id");
            d.current = field<bool>(s, "current");
            d.awaitingTwoFactor = optField<bool>(s, "awaitingTwoFactor").value_or(false);
            d.expiresOn = field<std::string>(s, "expiresOn");
            const auto label = s.find("label");
            if (label != s.end() && label->is_object()) d.label = sealed(*label);
            out.push_back(std::move(d));
        }
        return out;
    });
}

void setSessionLabel(ApiClient& api, const SealedSignal& label) {
    api.call("PUT", "auth/sessions/current/label", toJson(label));
}

void signOutSession(ApiClient& api, const std::string& id) {
    api.call("DELETE", "auth/sessions/" + urlEncode(id));
}

namespace {

LogCheckpoint checkpoint(const Json& c) {
    return LogCheckpoint{field<std::int64_t>(c, "treeSize"), field<std::string>(c, "rootHash"), field<std::string>(c, "signedAt"),
                         field<std::string>(c, "signature"), field<std::string>(c, "publicKey")};
}

}  // namespace

std::optional<LogCheckpoint> latestCheckpoint(ApiClient& api) {
    const Json j = api.call("GET", "public/transparency-log/checkpoint");
    return parse(j, [](const Json& body) -> std::optional<LogCheckpoint> {
        const auto it = body.find("checkpoint");
        if (it == body.end() || it->is_null()) return std::nullopt;
        return checkpoint(*it);
    });
}

std::vector<LogEntry> logEntries(ApiClient& api, std::optional<std::int64_t> before, int limit, const std::string& search) {
    std::string path = "public/transparency-log/entries?limit=" + std::to_string(limit);
    if (before) path += "&before=" + std::to_string(*before);
    if (!search.empty()) path += "&q=" + urlEncode(search);
    const Json j = api.call("GET", path);
    return parse(j, [](const Json& body) {
        std::vector<LogEntry> out;
        for (const auto& e : body) {
            out.push_back(LogEntry{field<std::int64_t>(e, "index"), field<std::string>(e, "commitment"), field<int>(e, "sizeKb"),
                                   field<std::int64_t>(e, "checkpointSize"), optField<std::string>(e, "loggedAt").value_or("")});
        }
        return out;
    });
}

InclusionProof inclusionProof(ApiClient& api, std::int64_t index, std::int64_t treeSize) {
    const Json j = api.call("GET", "public/transparency-log/entries/" + std::to_string(index) + "/proof?treeSize=" + std::to_string(treeSize));
    return parse(j, [](const Json& p) {
        return InclusionProof{field<std::int64_t>(p, "index"), field<std::int64_t>(p, "treeSize"), field<std::string>(p, "leafHash"),
                              field<std::vector<std::string>>(p, "path")};
    });
}

std::vector<std::string> consistencyProof(ApiClient& api, std::int64_t from, std::int64_t to) {
    const Json j = api.call("GET", "public/transparency-log/consistency?from=" + std::to_string(from) + "&to=" + std::to_string(to));
    return parse(j, [](const Json& p) { return field<std::vector<std::string>>(p, "path"); });
}

}  // namespace redvault::api::live

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

// An in-memory stand-in for the server's chat, friend, group and file
// routes, speaking the same JSON. Like the real server it only ever holds
// ciphertext, sealed keys and opaque ids; tests put plaintext in through the
// real crypto and check it comes back out on the other side.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/commitment.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/util/text.hpp"

namespace redvault::test {

using Json = nlohmann::json;

struct TestUser {
    std::string id;
    std::string username;
    crypto::KeyPair keys;

    data::SelfIdentity identity() const {
        return data::SelfIdentity{.handle = username,
                                  .userId = id,
                                  .publicKey = keys.publicKey,
                                  .privateKey = crypto::SecretBytes::copyOf(keys.privateKey),
                                  .role = "user",
                                  .subscriptionTier = "pro"};
    }
    std::string publicKeyB64() const { return crypto::base64Encode(keys.publicKey); }
};

inline TestUser makeUser(const std::string& id, const std::string& username) {
    return TestUser{id, username, crypto::generateKeyPair()};
}

class FakeChatServer {
public:
    static inline const std::string kBase = "https://example.com/api/";
    static inline const std::string kBucket = "https://bucket.example.net/";

    // --- State the tests arrange and inspect ---
    std::map<std::string, Json> users;                           // userId -> UserResult
    std::vector<Json> conversations;                             // what GET /conversations returns
    std::map<std::string, std::vector<Json>> participants;       // conversationId -> ParticipantDto[]
    std::map<std::string, std::vector<Json>> messages;           // conversationId -> MessageDto[]
    std::map<std::string, std::vector<Json>> receipts;           // conversationId -> ReceiptDto[]
    std::vector<std::pair<std::string, Json>> received;          // (path, body) of every POST/PATCH/PUT JSON call
    std::map<std::string, std::string> blobs;                    // objectId -> ciphertext
    std::vector<std::string> released;                           // objectIds POSTed to /release, in order
    std::vector<net::HttpRequest> transfers;                     // raw file PUT/GET requests

    std::string transferMode = "proxy";
    int transferFailuresToServe = 0;  // answer the next N transfers with 503
    bool corruptDownloads = false;
    bool tamperCommitments = false;
    std::string expiresAt = "2999-01-01T00:00:00.000Z";
    int messageListCalls = 0;
    /// Who "this app" is: the sender of its messages, and whose sealed key a rotation keeps for it.
    std::string selfId = "me";
    /// The last signing key registered with PUT /auth/signing-key (db/migrations/030).
    std::optional<std::string> signingKey;

    void addUser(const TestUser& user) {
        users[user.id] = Json{{"userId", user.id},
                              {"username", user.username},
                              {"publicKey", user.publicKeyB64()},
                              {"avatarUrl", nullptr},
                              {"createdAt", "2026-01-01T00:00:00.000Z"},
                              {"deleted", false}};
    }

    /// A chat between `self` and `others`, its key sealed to `self`. Returns the key.
    crypto::SecretBytes addConversation(const std::string& id, const TestUser& self, const std::vector<const TestUser*>& others,
                                        bool isGroup = false, const std::optional<std::string>& name = std::nullopt) {
        auto key = crypto::generateConversationKey();
        Json dto{{"id", id},
                 {"isGroup", isGroup},
                 {"role", "owner"},
                 {"wrappedKey", crypto::base64Encode(crypto::sealConversationKey(key, self.keys.publicKey))},
                 {"disappearingSeconds", nullptr},
                 {"createdBy", self.id},
                 {"createdAt", "2026-01-01T00:00:00.000Z"}};
        if (name) {
            const auto encrypted = crypto::encryptWithConversationKey(*name, key);
            dto["nameCiphertext"] = crypto::base64Encode(encrypted.ciphertext);
            dto["nameNonce"] = crypto::base64Encode(encrypted.nonce);
        }
        conversations.push_back(dto);
        participants[id].push_back(Json{{"userId", self.id}, {"role", "owner"}, {"joinedAt", "2026-01-01T00:00:00.000Z"}});
        for (const auto* other : others) {
            participants[id].push_back(
                Json{{"userId", other->id}, {"role", "member"}, {"joinedAt", "2026-01-01T00:00:00.000Z"}});
        }
        return key;
    }

    /// Stores a message encrypted with `key`, as if `senderId` had sent `plaintext`.
    Json& addMessage(const std::string& conversationId, const std::string& id, const std::string& senderId,
                     const std::string& plaintext, crypto::ByteView key, const std::string& createdAt) {
        const auto encrypted = crypto::encryptWithConversationKey(plaintext, key);
        messages[conversationId].push_back(Json{{"id", id},
                                                {"conversationId", conversationId},
                                                {"senderId", senderId},
                                                {"ciphertext", crypto::base64Encode(encrypted.ciphertext)},
                                                {"nonce", crypto::base64Encode(encrypted.nonce)},
                                                {"createdAt", createdAt},
                                                {"attachments", Json::array()}});
        return messages[conversationId].back();
    }

    std::vector<Json> bodiesFor(const std::string& path) const {
        std::vector<Json> out;
        for (const auto& [p, body] : received) {
            if (p == path) out.push_back(body);
        }
        return out;
    }

    net::HttpResponse handle(const net::HttpRequest& request) {
        if (request.url.starts_with(kBucket)) return transfer(request, request.url.substr(kBucket.size()));
        const std::string pathAndQuery = request.url.substr(kBase.size());
        const std::string path = pathAndQuery.substr(0, pathAndQuery.find('?'));
        const auto parts = split(path);
        const Json body = request.body.empty() ? Json() : Json::parse(request.body, nullptr, false);
        if (request.method != "GET" && !body.is_discarded() && !request.body.empty()) received.emplace_back(path, body);
        const std::string& m = request.method;

        if (m == "GET" && path == "conversations") return ok(Json(conversations));
        if (parts.size() == 3 && parts[0] == "conversations") {
            const std::string& id = parts[1];
            if (m == "GET" && parts[2] == "participants") return ok(Json(participants[id]));
            if (m == "GET" && parts[2] == "messages") return ok(messagePage(id, pathAndQuery));
            if (m == "GET" && parts[2] == "receipts") return ok(Json(receipts[id]));
            if (m == "POST" && parts[2] == "messages") return sendMessage(id, body);
            if (m == "POST" && parts[2] == "read") return noContent();
            if (m == "PATCH" && parts[2] == "disappearing") return noContent();
            if (m == "PATCH" && parts[2] == "name") return noContent();
            if (m == "POST" && parts[2] == "participants") {
                if (Json* dto = conversation(id); dto != nullptr && body.contains("keyNotice")) (*dto)["keyNotice"] = body["keyNotice"];
                return noContent();
            }
            if (m == "PUT" && parts[2] == "key-notice") {
                if (Json* dto = conversation(id)) (*dto)["keyNotice"] = body;
                return noContent();
            }
            if (m == "POST" && parts[2] == "keys") return rotateKey(id, body);
        }
        if (m == "PUT" && path == "auth/signing-key") {
            signingKey = body["signingPublicKey"].get<std::string>();
            if (users.contains(selfId)) {
                users[selfId]["signingPublicKey"] = *signingKey;
                users[selfId]["signingKeySince"] = "2026-01-01";
            }
            return noContent();
        }
        if (m == "POST" && path == "conversations") {
            return ok(Json{{"id", "g-new"}, {"isGroup", true}, {"createdAt", "2026-01-01T00:00:00.000Z"}});
        }
        if (m == "GET" && parts.size() == 2 && parts[0] == "users") {
            const auto it = users.find(parts[1]);
            if (it == users.end()) return error(404, "not_found");
            return ok(it->second);
        }
        if (m == "GET" && parts.size() == 2 && parts[0] == "messages") return error(404, "not_found");
        if (parts.size() == 3 && parts[0] == "friend-requests" && parts[2] == "accept") {
            return ok(Json{{"id", "c-accepted"}, {"isGroup", false}, {"createdAt", "2026-01-01T00:00:00.000Z"}});
        }

        // Files
        if (m == "GET" && path == "v1/files/config") {
            return ok(Json{{"maxFileSize", 1024 * 1024},
                           {"maxEncryptedSize", 1024 * 1024 + 1024},
                           {"transferMode", transferMode}});
        }
        if (m == "POST" && path == "v1/files/upload-request") {
            const std::string objectId = util::randomUuid();
            pendingSizes[objectId] = body["encryptedSize"].get<std::int64_t>();
            return ok(Json{{"objectId", objectId}, {"maxEncryptedSize", 2 * 1024 * 1024}, {"upload", target(objectId, "PUT")}});
        }
        if (parts.size() == 4 && parts[0] == "v1" && parts[1] == "files") {
            const std::string& objectId = parts[2];
            if (m == "PUT" && parts[3] == "content") return transfer(request, objectId);
            if (m == "GET" && parts[3] == "content") return transfer(request, objectId);
            if (m == "POST" && parts[3] == "complete") {
                if (!blobs.contains(objectId)) return error(409, "upload_incomplete");
                return ok(Json{{"objectId", objectId}, {"status", "available"}, {"encryptedSize", blobs[objectId].size()}});
            }
            if (m == "POST" && parts[3] == "release") {
                if (!blobs.contains(objectId)) return error(404, "not_found");
                released.push_back(objectId);
                return ok(Json{{"objectId", objectId}, {"status", "available"}});
            }
            if (m == "GET" && parts[3] == "download") {
                if (!blobs.contains(objectId)) return error(404, "not_found");
                return ok(Json{{"objectId", objectId}, {"encryptedSize", blobs[objectId].size()}, {"download", target(objectId, "GET")}});
            }
        }
        if (m == "DELETE" && parts.size() == 3 && parts[0] == "v1" && parts[1] == "files") {
            if (blobs.erase(parts[2]) == 0) return error(404, "not_found");
            return net::HttpResponse{.status = 204};
        }
        return error(404, "not_found");
    }

    /// Wires `transport` to this server.
    void attach(FakeTransport& transport) {
        transport.handler = [this](const net::HttpRequest& request) { return handle(request); };
    }

private:
    std::map<std::string, std::int64_t> pendingSizes;
    int nextMessage = 0;

    static std::vector<std::string> split(const std::string& path) {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= path.size()) {
            const auto slash = path.find('/', start);
            out.push_back(path.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return out;
    }

    static std::optional<std::string> queryParam(const std::string& pathAndQuery, const std::string& name) {
        const auto q = pathAndQuery.find('?');
        if (q == std::string::npos) return std::nullopt;
        std::size_t start = q + 1;
        while (start < pathAndQuery.size()) {
            const auto amp = pathAndQuery.find('&', start);
            const std::string pair = pathAndQuery.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
            if (pair.starts_with(name + "=")) {
                std::string value;
                const std::string raw = pair.substr(name.size() + 1);
                for (std::size_t i = 0; i < raw.size(); ++i) {
                    if (raw[i] == '%' && i + 2 < raw.size()) {
                        value.push_back(static_cast<char>(std::stoi(raw.substr(i + 1, 2), nullptr, 16)));
                        i += 2;
                    } else {
                        value.push_back(raw[i]);
                    }
                }
                return value;
            }
            if (amp == std::string::npos) break;
            start = amp + 1;
        }
        return std::nullopt;
    }

    /// Like the server: newest first, `before` exclusive, `limit` (default 50).
    Json messagePage(const std::string& conversationId, const std::string& pathAndQuery) {
        ++messageListCalls;
        std::vector<Json> all = messages[conversationId];
        std::stable_sort(all.begin(), all.end(), [](const Json& a, const Json& b) {
            return util::parseIsoMillis(a["createdAt"].get<std::string>()) >
                   util::parseIsoMillis(b["createdAt"].get<std::string>());
        });
        const auto limit = queryParam(pathAndQuery, "limit");
        const auto before = queryParam(pathAndQuery, "before");
        const std::size_t max = limit ? std::stoul(*limit) : 50;
        Json out = Json::array();
        for (const auto& message : all) {
            if (out.size() >= max) break;
            if (before && util::parseIsoMillis(message["createdAt"].get<std::string>()) >= util::parseIsoMillis(*before)) {
                continue;
            }
            out.push_back(message);
        }
        return out;
    }

    static net::HttpResponse ok(const Json& body) { return {200, {}, body.dump()}; }
    static net::HttpResponse noContent() { return {204, {}, ""}; }
    static net::HttpResponse error(int status, const std::string& code) {
        return {status, {}, Json{{"error", code}, {"message", code}}.dump()};
    }

    Json target(const std::string& objectId, const std::string& method) const {
        const bool proxy = transferMode == "proxy";
        return Json{{"mode", transferMode},
                    {"url", proxy ? "/v1/files/" + objectId + "/content" : kBucket + objectId},
                    {"method", method},
                    {"headers", Json::object()},
                    {"expiresAt", expiresAt}};
    }

    Json* conversation(const std::string& id) {
        for (auto& dto : conversations) {
            if (dto["id"] == id) return &dto;
        }
        return nullptr;
    }

    /// Like the server (db/migrations/030): only from the current epoch, then the next one for everyone listed.
    net::HttpResponse rotateKey(const std::string& conversationId, const Json& body) {
        Json* dto = conversation(conversationId);
        if (dto == nullptr) return error(404, "not_found");
        const int epoch = dto->value("keyEpoch", 0);
        if (body["fromEpoch"].get<int>() != epoch) return error(409, "stale_key_epoch");
        if (!dto->contains("keys")) (*dto)["keys"] = Json::array({Json{{"epoch", epoch}, {"wrappedKey", (*dto)["wrappedKey"]}}});
        for (const auto& key : body["keys"]) {
            if (key["userId"] == selfId) {
                (*dto)["wrappedKey"] = key["wrappedKey"];
                (*dto)["keys"].push_back(Json{{"epoch", epoch + 1}, {"wrappedKey", key["wrappedKey"]}});
            }
        }
        (*dto)["keyEpoch"] = epoch + 1;
        (*dto)["rekeyRequired"] = false;
        if (body.contains("keyNotice")) (*dto)["keyNotice"] = body["keyNotice"];
        if (body.contains("nameCiphertext")) {
            (*dto)["nameCiphertext"] = body["nameCiphertext"];
            (*dto)["nameNonce"] = body["nameNonce"];
        }
        return ok(Json{{"keyEpoch", epoch + 1}});
    }

    net::HttpResponse sendMessage(const std::string& conversationId, const Json& body) {
        if (const Json* dto = conversation(conversationId)) {
            if (dto->value("rekeyRequired", false)) return error(409, "group_rekey_required");
            if (body.contains("keyEpoch") && body["keyEpoch"].get<int>() != dto->value("keyEpoch", 0)) {
                return error(409, "stale_key_epoch");
            }
        }
        // Same attachment again returns the original message, like the real server.
        const std::string id = "00000000-0000-4000-8000-" + std::string(12 - std::to_string(nextMessage).size(), '0') +
                               std::to_string(nextMessage);
        ++nextMessage;
        const auto ciphertext = crypto::base64Decode(body["ciphertext"].get<std::string>());
        const auto nonce = crypto::base64Decode(body["nonce"].get<std::string>());
        std::string commitment = crypto::commitment::computeHex(id, ciphertext, nonce);
        if (tamperCommitments) commitment[0] = commitment[0] == 'a' ? 'b' : 'a';
        Json attachments = Json::array();
        if (body.contains("attachments")) {
            for (const auto& objectId : body["attachments"]) {
                attachments.push_back(Json{{"objectId", objectId}, {"status", "available"}});
            }
        }
        Json dto{{"id", id},
                 {"conversationId", conversationId},
                 {"senderId", selfId},
                 {"keyEpoch", body.value("keyEpoch", 0)},
                 {"signature", body.contains("signature") ? body["signature"] : Json()},
                 {"ciphertext", body["ciphertext"]},
                 {"nonce", body["nonce"]},
                 {"createdAt", "2026-09-25T10:00:00.000Z"},
                 {"deleteAt", "2027-03-25T10:00:00.000Z"},
                 {"attachments", attachments},
                 {"verification",
                  {{"publicTransactionId", "tx-" + id},
                   {"commitment", commitment},
                   {"protocolVersion", 1},
                   {"relayStatus", "relayed"},
                   {"createdAt", "2026-09-25T10:00:00.000Z"}}}};
        messages[conversationId].push_back(dto);
        return ok(dto);
    }

    net::HttpResponse transfer(const net::HttpRequest& request, const std::string& objectId) {
        transfers.push_back(request);
        if (transferFailuresToServe > 0) {
            --transferFailuresToServe;
            return {503, {}, "busy"};
        }
        if (request.method == "PUT") {
            if (static_cast<std::int64_t>(request.body.size()) != pendingSizes[objectId]) return error(400, "size_mismatch");
            blobs[objectId] = request.body;
            return noContent();
        }
        const auto it = blobs.find(objectId);
        if (it == blobs.end()) return {404, {}, ""};
        std::string body = it->second;
        if (corruptDownloads && !body.empty()) body[body.size() / 2] ^= 0x01;
        return {200, {}, body};
    }
};

}  // namespace redvault::test

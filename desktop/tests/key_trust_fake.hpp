#pragma once

// A fake server that knows several people's apps at once, for key trust: each person sees their own sealed copies
// of a chat's keys, and notices are stored the way the real server stores them. Shared by key_trust_flow_test.cpp and
// the app tests, so the same world drives the repositories and the real ChatSession.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "redvault/api/dtos.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/net/http.hpp"

namespace redvault::test::keytrust {

using Json = nlohmann::json;
using crypto::base64Encode;
using data::SelfIdentity;

const std::string kDmId = "d0000000-0000-4000-8000-00000000000d";
const std::string kGroupId = "f0000000-0000-4000-8000-00000000000f";
const std::string kBase = "https://example.com/api/";
const std::string kRefused = "Couldn't decrypt this message.";

inline std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

struct Person {
    std::string userId;
    crypto::KeyPair keys = crypto::generateKeyPair();
    std::optional<std::string> signingKey;
    std::optional<std::string> since;

    SelfIdentity identity() const {
        return SelfIdentity{.handle = "h", .userId = userId, .publicKey = keys.publicKey, .privateKey = crypto::SecretBytes::copyOf(keys.privateKey),
                            .role = "user", .subscriptionTier = "free"};
    }
    crypto::SigningKeyPair signer() const { return crypto::deriveSigningKeyPair(keys.privateKey); }
};

/// One conversation as the server stores it; `wrapped` is each person's sealed copies, by epoch.
struct Chat {
    std::string id;
    bool isGroup = false;
    std::vector<std::string> members;
    int keyEpoch = 0;
    bool rekeyRequired = false;
    std::map<std::string, std::map<int, std::string>> wrapped;
    std::optional<Json> keyNotice;
    std::optional<Json> community;
    std::vector<Json> messages;

    void seal(const Person& person, int epoch, crypto::ByteView key) {
        wrapped[person.userId][epoch] = base64Encode(crypto::sealConversationKey(key, person.keys.publicKey));
    }

    Json dto(const std::string& userId) const {
        const auto& mine = wrapped.at(userId);
        Json keys = Json::array();
        for (const auto& [epoch, wrappedKey] : mine) keys.push_back(Json{{"epoch", epoch}, {"wrappedKey", wrappedKey}});
        Json out{{"id", id}, {"isGroup", isGroup}, {"role", "owner"}, {"wrappedKey", mine.at(keyEpoch)}, {"keyEpoch", keyEpoch},
                 {"keys", keys}, {"rekeyRequired", rekeyRequired}, {"keyNotice", keyNotice ? *keyNotice : Json(nullptr)},
                 {"createdBy", members.front()}, {"createdAt", "2026-10-01T00:00:00.000Z"}};
        if (community) out["community"] = *community;
        return out;
    }
};

struct Server {
    std::string caller;
    std::map<std::string, Person*> people;
    std::map<std::string, Chat*> chats;
    std::vector<std::string> requests;
    std::optional<int> failNotice;
    bool staleNotice = false;
    bool failAdd = false;
    std::string inviteConversation;
    /// Asked in capitals, the server answers with whatever key suits it.
    std::optional<std::string> forgedSigningKey;
    /// By user id: the public key the server hands out instead of the real one.
    std::map<std::string, std::string> forgedPublicKeys;
    bool saysNothingAboutSigning = false;
    bool answersFailForUsers = false;
    int seq = 0;

    static net::HttpResponse json(const Json& body) { return {200, {}, body.dump()}; }
    static net::HttpResponse error(int status, const std::string& code) { return {status, {}, Json{{"error", code}, {"message", code}}.dump()}; }
    static net::HttpResponse noContent() { return {204, {}, ""}; }

    int sent(const std::string& fragment) const {
        return static_cast<int>(std::count_if(requests.begin(), requests.end(), [&](const std::string& r) { return r.find(fragment) != std::string::npos; }));
    }

    Json message(const Chat& chat, const std::string& senderId, const std::string& ciphertext, const std::string& nonce,
                 const std::optional<std::string>& signature, std::optional<int> epoch = std::nullopt, std::optional<std::string> createdAt = std::nullopt) {
        ++seq;
        char id[64];
        std::snprintf(id, sizeof id, "e0000000-0000-4000-8000-%012d", seq);
        char at[64];
        std::snprintf(at, sizeof at, "2026-10-02T00:00:%02d.000Z", seq);
        return Json{{"id", id}, {"conversationId", chat.id}, {"senderId", senderId}, {"ciphertext", ciphertext}, {"nonce", nonce},
                    {"createdAt", createdAt.value_or(at)}, {"keyEpoch", epoch.value_or(chat.keyEpoch)}, {"signature", signature ? Json(*signature) : Json(nullptr)}};
    }

    net::HttpResponse handle(const net::HttpRequest& r) {
        const std::string full = r.url.substr(kBase.size());
        const std::string path = full.substr(0, full.find('?'));
        requests.push_back(r.method + " " + path + " " + r.body);
        const Json body = r.body.empty() ? Json() : Json::parse(r.body);
        const auto slash = path.find('/', 14);
        const std::string chatId = path.starts_with("conversations/") ? path.substr(14, slash == std::string::npos ? std::string::npos : slash - 14) : "";
        Chat* chat = chats.contains(chatId) ? chats[chatId] : nullptr;
        const std::string under = "conversations/" + chatId;
        if (r.method == "GET" && path == "conversations") {
            Json out = Json::array();
            for (const auto& [id, c] : chats) {
                if (std::find(c->members.begin(), c->members.end(), caller) != c->members.end()) out.push_back(c->dto(caller));
            }
            return json(out);
        }
        if (r.method == "GET" && path == under + "/participants") {
            Json out = Json::array();
            for (const auto& member : chat->members) out.push_back(Json{{"userId", member}, {"role", "member"}, {"joinedAt", "2026-10-01T00:00:00.000Z"}});
            return json(out);
        }
        if (r.method == "GET" && path.starts_with("users/")) {
            const std::string id = path.substr(6);
            std::string lower = id;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (answersFailForUsers) return error(503, "unavailable");
            const auto it = people.find(lower);
            if (it == people.end()) return error(404, "not_found");
            const Person& p = *it->second;
            const auto signing = saysNothingAboutSigning ? std::nullopt : id != lower ? forgedSigningKey : p.signingKey;
            const auto since = saysNothingAboutSigning ? std::nullopt : p.since;
            const auto forged = forgedPublicKeys.find(p.userId);
            return json(Json{{"userId", p.userId}, {"username", "u"}, {"publicKey", forged != forgedPublicKeys.end() ? forged->second : base64Encode(p.keys.publicKey)},
                             {"createdAt", "2026-10-01T00:00:00.000Z"}, {"signingPublicKey", signing ? Json(*signing) : Json(nullptr)},
                             {"signingKeySince", since ? Json(*since) : Json(nullptr)}});
        }
        if (r.method == "POST" && path == "conversations") {
            return json(Json{{"id", "99990000-0000-4000-8000-000000000001"}, {"isGroup", body["isGroup"]}, {"createdAt", "2026-10-01T00:00:00.000Z"}});
        }
        if (r.method == "POST" && path.starts_with("friend-requests/") && path.ends_with("/accept")) {
            return json(Json{{"id", "99990000-0000-4000-8000-000000000002"}, {"isGroup", false}, {"createdAt", "2026-10-01T00:00:00.000Z"}});
        }
        if (r.method == "POST" && path.starts_with("backup/") && path.ends_with("/shares")) return noContent();
        if (r.method == "PUT" && path == "auth/signing-key") {
            Person& p = *people.at(caller);
            p.signingKey = body["signingPublicKey"].get<std::string>();
            if (!p.since) p.since = "2026-10-01";
            return noContent();
        }
        if (r.method == "PUT" && path == under + "/key-notice") {
            if (failNotice) return error(*failNotice, "unavailable");
            if (staleNotice) return error(409, "stale_key_notice");
            chat->keyNotice = body;
            return noContent();
        }
        if (r.method == "POST" && path == under + "/keys") {
            if (body["fromEpoch"].get<int>() != chat->keyEpoch) return error(409, "stale_key_epoch");
            std::vector<std::string> given;
            for (const auto& k : body["keys"]) given.push_back(k["userId"].get<std::string>());
            std::vector<std::string> members = chat->members;
            std::sort(given.begin(), given.end());
            std::sort(members.begin(), members.end());
            if (given != members) return error(409, "membership_changed");
            if (chat->keyNotice && !body.contains("keyNotice")) return error(426, "client_update_required");
            ++chat->keyEpoch;
            chat->rekeyRequired = false;
            for (const auto& k : body["keys"]) chat->wrapped[k["userId"].get<std::string>()][chat->keyEpoch] = k["wrappedKey"].get<std::string>();
            if (body.contains("keyNotice")) chat->keyNotice = body["keyNotice"];
            return json(Json{{"keyEpoch", chat->keyEpoch}});
        }
        if (r.method == "POST" && path == under + "/participants") {
            if (failAdd) return error(403, "not_addable");
            const auto userId = body["userId"].get<std::string>();
            chat->members.push_back(userId);
            chat->wrapped[userId][body["keyEpoch"].get<int>()] = body["wrappedKey"].get<std::string>();
            if (body.contains("keyNotice")) chat->keyNotice = body["keyNotice"];
            return noContent();
        }
        if (r.method == "POST" && path.starts_with("group-invites/groups/") && path.ends_with("/approve")) {
            if (failAdd) return error(403, "not_addable");
            std::vector<std::string> parts;
            for (std::size_t start = 0, end = 0; end != std::string::npos; start = end + 1) {
                end = path.find('/', start);
                parts.push_back(path.substr(start, end == std::string::npos ? std::string::npos : end - start));
            }
            Chat& approving = *chats.at(parts[2]);
            const std::string& userId = parts[4];
            approving.members.push_back(userId);
            approving.wrapped[userId][body["keyEpoch"].get<int>()] = body["wrappedKey"].get<std::string>();
            if (body.contains("keyNotice")) approving.keyNotice = body["keyNotice"];
            return noContent();
        }
        if (r.method == "POST" && path.ends_with("/join")) {
            Chat& joining = *chats.at(inviteConversation);
            joining.members.push_back(caller);
            joining.wrapped[caller][joining.keyEpoch] = body["wrappedKey"].get<std::string>();
            return json(Json{{"status", "joined"}, {"conversationId", inviteConversation}});
        }
        if (r.method == "POST" && path == under + "/messages") {
            if (chat->rekeyRequired) return error(409, "group_rekey_required");
            Json stored = message(*chat, caller, body["ciphertext"].get<std::string>(), body["nonce"].get<std::string>(),
                                  body.contains("signature") ? std::optional<std::string>(body["signature"].get<std::string>()) : std::nullopt);
            chat->messages.push_back(stored);
            return json(stored);
        }
        if (r.method == "GET" && path == under + "/messages") {
            Json out = Json::array();
            std::vector<Json> newestFirst = chat->messages;
            std::stable_sort(newestFirst.begin(), newestFirst.end(), [](const Json& a, const Json& b) { return a["createdAt"].get<std::string>() > b["createdAt"].get<std::string>(); });
            for (const auto& m : newestFirst) out.push_back(m);
            return json(out);
        }
        if (r.method == "GET" && path == under + "/receipts") return json(Json::array());
        if (r.method == "POST" && path == under + "/read") return noContent();
        return error(404, "not_found");
    }
};

struct World {
    Server server;
    Person alice{"a0000000-0000-4000-8000-000000000001"};
    Person bob{"b0000000-0000-4000-8000-000000000002"};
    Person carol{"c0000000-0000-4000-8000-000000000003"};
    Person eve{"e0000000-0000-4000-8000-000000000004"};
    Person dave{"d1000000-0000-4000-8000-000000000005"};
    crypto::SecretBytes dmKey = crypto::generateConversationKey();
    crypto::SecretBytes groupKey = crypto::generateConversationKey();
    Chat dm{kDmId, false};
    Chat group{kGroupId, true};

    World() {
        for (Person* p : {&alice, &bob, &carol, &eve, &dave}) server.people[p->userId] = p;
        server.chats[kDmId] = &dm;
        server.chats[kGroupId] = &group;
        for (const Person* p : {&alice, &bob}) {
            dm.members.push_back(p->userId);
            dm.seal(*p, 0, dmKey);
        }
        for (const Person* p : {&alice, &bob, &carol}) {
            group.members.push_back(p->userId);
            group.seal(*p, 0, groupKey);
        }
    }

    Json signedNotice(const crypto::SigningKeyPair& signer, const std::string& issuer, const Chat& chat, std::int64_t epoch, std::int64_t seq,
                      const std::vector<std::string>& members, crypto::ByteView key) const {
        const auto input = crypto::keyNoticeInput(chat.id, issuer, epoch, seq, members, crypto::keyCommitment(key));
        return Json(api::KeyNoticeDto{epoch, seq, issuer, members, base64Encode(crypto::signMessage(input, signer.secretKey))});
    }

    api::KeyNoticeDto storedNotice() const { return group.keyNotice->get<api::KeyNoticeDto>(); }
};


}  // namespace redvault::test::keytrust

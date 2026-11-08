// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/api/story_api.hpp"

#include <nlohmann/json.hpp>

namespace redvault::api::stories {

namespace {

using net::percentEncode;

template <typename F>
auto parse(const Json& j, F&& f) -> decltype(f(j)) {
    try {
        return f(j);
    } catch (const Json::exception& e) {
        throw ApiError(0, "malformed_response", std::string("Unexpected response: ") + e.what());
    }
}

Story storyFrom(const Json& j) {
    Story s;
    s.id = j.at("id").get<std::string>();
    s.authorId = j.at("authorId").get<std::string>();
    s.kind = j.at("kind").get<std::string>();
    s.metaCiphertext = j.at("metaCiphertext").get<std::string>();
    s.metaNonce = j.at("metaNonce").get<std::string>();
    if (const auto it = j.find("mediaSize"); it != j.end() && it->is_number_integer()) s.mediaSize = it->get<std::uint64_t>();
    s.hasMedia = j.value("hasMedia", false);
    s.allowReplies = j.value("allowReplies", true);
    s.createdAt = j.value("createdAt", std::string());
    s.expiresAt = j.value("expiresAt", std::string());
    s.wrappedKey = j.at("wrappedKey").get<std::string>();
    s.seen = j.value("seen", false);
    s.viewCount = j.value("viewCount", 0);
    if (const auto it = j.find("archivedAt"); it != j.end() && it->is_string()) s.archivedAt = it->get<std::string>();
    return s;
}

std::vector<Story> storyList(const Json& list) {
    std::vector<Story> out;
    for (const auto& s : list) out.push_back(storyFrom(s));
    return out;
}

Json keysJson(const std::vector<WrappedKey>& keys) {
    Json out = Json::array();
    for (const auto& k : keys) out.push_back({{"userId", k.userId}, {"wrappedKey", k.wrappedKey}});
    return out;
}

std::string storyPath(const std::string& id, const char* rest = "") {
    return "stories/" + percentEncode(id) + rest;
}

}  // namespace

Config config(ApiClient& api) {
    return parse(api.call("GET", "stories/config"), [](const Json& j) {
        Config c;
        c.media = j.value("media", false);
        // Missing on servers that predate interactive stories: treated as off.
        if (const auto it = j.find("interactive"); it != j.end() && it->is_boolean()) c.interactive = it->get<bool>();
        if (const auto it = j.find("ttlSeconds"); it != j.end() && it->is_array()) {
            for (const auto& v : *it) {
                if (v.is_number_integer() && v.get<std::int64_t>() > 0 && v.get<std::int64_t>() <= 30LL * 86400) c.ttlSeconds.push_back(v.get<int>());
            }
        }
        return c;
    });
}

bool mediaEnabled(ApiClient& api) {
    return config(api).media;
}

Feed feed(ApiClient& api) {
    return parse(api.call("GET", "stories/feed"), [](const Json& j) {
        Feed f;
        f.own = storyList(j.at("own"));
        for (const auto& a : j.at("authors")) f.authors.push_back({a.at("authorId").get<std::string>(), storyList(a.at("stories"))});
        return f;
    });
}

bool create(ApiClient& api, const NewStory& story) {
    Json body{{"id", story.id},
              {"kind", story.kind},
              {"metaCiphertext", story.metaCiphertext},
              {"metaNonce", story.metaNonce},
              {"mediaSize", story.mediaSize ? Json(*story.mediaSize) : Json(nullptr)},
              {"audience", story.audience},
              {"allowReplies", story.allowReplies},
              {"keys", keysJson(story.keys)}};
    if (story.ttlSeconds) body["ttlSeconds"] = *story.ttlSeconds;
    return parse(api.call("POST", "stories", body), [](const Json& j) { return j.value("published", false); });
}

void uploadMedia(ApiClient& api, const std::string& storyId, std::string ciphertext) {
    api.callRaw("PUT", storyPath(storyId, "/media"), std::move(ciphertext), "application/octet-stream");
}

std::string downloadMedia(ApiClient& api, const std::string& storyId) {
    return api.callRaw("GET", storyPath(storyId, "/media"));
}

void markViewed(ApiClient& api, const std::string& storyId) {
    api.call("POST", storyPath(storyId, "/view"));
}

std::vector<std::string> viewers(ApiClient& api, const std::string& storyId) {
    return parse(api.call("GET", storyPath(storyId, "/viewers")), [](const Json& j) { return j.get<std::vector<std::string>>(); });
}

void remove(ApiClient& api, const std::string& storyId) {
    api.call("DELETE", storyPath(storyId));
}

std::vector<Story> archive(ApiClient& api) {
    return parse(api.call("GET", "stories/archive"), storyList);
}

std::vector<KeyGap> keyGaps(ApiClient& api) {
    return parse(api.call("GET", "stories/key-gaps"), [](const Json& list) {
        std::vector<KeyGap> out;
        for (const auto& g : list) {
            KeyGap gap{g.at("storyId").get<std::string>(), {}};
            for (const auto& u : g.at("users")) gap.users.emplace_back(u.at("userId").get<std::string>(), u.at("publicKey").get<std::string>());
            out.push_back(std::move(gap));
        }
        return out;
    });
}

int addKeys(ApiClient& api, const std::string& storyId, const std::vector<WrappedKey>& keys) {
    return parse(api.call("POST", storyPath(storyId, "/keys"), Json{{"keys", keysJson(keys)}}), [](const Json& j) { return j.value("added", 0); });
}

int revoke(ApiClient& api, const std::string& userId, const std::optional<std::vector<std::string>>& storyIds) {
    Json body{{"userId", userId}};
    if (storyIds) body["storyIds"] = *storyIds;
    return parse(api.call("POST", "stories/revoke", body), [](const Json& j) { return j.value("removed", 0); });
}

int putAnswer(ApiClient& api, const std::string& storyId, const std::string& stickerId, const std::string& envelopeBase64, bool anonymous, bool final) {
    const Json body{{"envelope", envelopeBase64}, {"anonymous", anonymous}, {"final", final}};
    return parse(api.call("PUT", storyPath(storyId, "/responses/") + percentEncode(stickerId), Json(body)), [](const Json& j) { return j.value("revision", 0); });
}

std::vector<AnswerRow> answers(ApiClient& api, const std::string& storyId) {
    return parse(api.call("GET", storyPath(storyId, "/responses")), [](const Json& list) {
        std::vector<AnswerRow> out;
        for (const auto& r : list) {
            AnswerRow row;
            row.stickerId = r.at("stickerId").get<std::string>();
            if (const auto it = r.find("responderId"); it != r.end() && it->is_string()) row.responderId = it->get<std::string>();
            row.anonymous = r.value("anonymous", false);
            row.envelope = r.at("envelope").get<std::string>();
            row.revision = r.value("revision", 0);
            row.respondedAt = r.value("respondedAt", std::string());
            out.push_back(std::move(row));
        }
        return out;
    });
}

std::vector<MyAnswerRow> myAnswers(ApiClient& api, const std::string& storyId) {
    return parse(api.call("GET", storyPath(storyId, "/responses/mine")), [](const Json& list) {
        std::vector<MyAnswerRow> out;
        for (const auto& r : list) {
            out.push_back({r.at("stickerId").get<std::string>(), r.value("anonymous", false), r.at("envelope").get<std::string>(), r.value("revision", 0)});
        }
        return out;
    });
}

void putTotals(ApiClient& api, const std::string& storyId, const std::string& stickerId, int revision, const std::string& nonce,
               const std::string& ciphertext) {
    api.call("PUT", storyPath(storyId, "/results/") + percentEncode(stickerId), Json{{"revision", revision}, {"nonce", nonce}, {"ciphertext", ciphertext}});
}

std::vector<TotalsRow> totals(ApiClient& api, const std::string& storyId) {
    return parse(api.call("GET", storyPath(storyId, "/results")), [](const Json& list) {
        std::vector<TotalsRow> out;
        for (const auto& r : list) {
            out.push_back({r.at("stickerId").get<std::string>(), r.value("revision", 0), r.at("nonce").get<std::string>(), r.at("ciphertext").get<std::string>()});
        }
        return out;
    });
}

void reportStory(ApiClient& api, const std::string& storyId, const std::string& reason, const std::optional<std::string>& evidence) {
    Json body{{"reason", reason}};
    if (evidence && !evidence->empty()) body["evidence"] = *evidence;
    api.call("POST", storyPath(storyId, "/report"), body);
}

void reportAnswer(ApiClient& api, const std::string& storyId, const std::string& stickerId, const std::string& envelopeBase64, const std::string& reason,
                  const std::optional<std::string>& evidence) {
    Json body{{"stickerId", stickerId}, {"envelope", envelopeBase64}, {"reason", reason}};
    if (evidence && !evidence->empty()) body["evidence"] = *evidence;
    api.call("POST", storyPath(storyId, "/responses/report"), body);
}

std::vector<Highlight> highlights(ApiClient& api, const std::string& userId) {
    return parse(api.call("GET", "stories/users/" + percentEncode(userId) + "/highlights"), [](const Json& list) {
        std::vector<Highlight> out;
        for (const auto& h : list) {
            out.push_back({h.at("id").get<std::string>(), h.at("ownerId").get<std::string>(), h.at("titleCiphertext").get<std::string>(),
                           h.at("titleNonce").get<std::string>(), h.at("coverStoryId").get<std::string>(), h.value("createdAt", std::string()),
                           storyList(h.at("stories"))});
        }
        return out;
    });
}

std::string createHighlight(ApiClient& api, const std::string& titleCiphertext, const std::string& titleNonce, const std::string& coverStoryId,
                            const std::vector<std::string>& storyIds) {
    Json body{{"titleCiphertext", titleCiphertext}, {"titleNonce", titleNonce}, {"coverStoryId", coverStoryId}, {"storyIds", storyIds}};
    return parse(api.call("POST", "stories/highlights", body), [](const Json& j) { return j.at("id").get<std::string>(); });
}

void updateHighlight(ApiClient& api, const std::string& highlightId, const HighlightInput& patch) {
    Json body = Json::object();
    if (patch.titleCiphertext) body["titleCiphertext"] = *patch.titleCiphertext;
    if (patch.titleNonce) body["titleNonce"] = *patch.titleNonce;
    if (patch.coverStoryId) body["coverStoryId"] = *patch.coverStoryId;
    if (patch.storyIds) body["storyIds"] = *patch.storyIds;
    api.call("PATCH", "stories/highlights/" + percentEncode(highlightId), body);
}

void removeHighlight(ApiClient& api, const std::string& highlightId) {
    api.call("DELETE", "stories/highlights/" + percentEncode(highlightId));
}

Prefs prefs(ApiClient& api) {
    return parse(api.call("GET", "stories/prefs"), [](const Json& j) {
        Prefs p;
        if (const auto it = j.find("prefsCiphertext"); it != j.end() && it->is_string()) p.prefsCiphertext = it->get<std::string>();
        if (const auto it = j.find("prefsNonce"); it != j.end() && it->is_string()) p.prefsNonce = it->get<std::string>();
        p.archiveEnabled = j.value("archiveEnabled", false);
        p.revision = j.value("revision", 0);
        return p;
    });
}

Prefs putPrefs(ApiClient& api, const std::string& ciphertext, const std::string& nonce, bool archiveEnabled, int revision) {
    Json body{{"prefsCiphertext", ciphertext}, {"prefsNonce", nonce}, {"archiveEnabled", archiveEnabled}, {"revision", revision}};
    return parse(api.call("PUT", "stories/prefs", body), [&](const Json& j) {
        Prefs p;
        p.prefsCiphertext = ciphertext;
        p.prefsNonce = nonce;
        p.archiveEnabled = j.value("archiveEnabled", archiveEnabled);
        p.revision = j.value("revision", revision + 1);
        return p;
    });
}

}  // namespace redvault::api::stories

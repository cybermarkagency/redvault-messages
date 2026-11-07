#include "redvault/api/group_invite_api.hpp"

#include <nlohmann/json.hpp>

namespace redvault::api::invites {

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

std::optional<std::string> optionalText(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    return it->get<std::string>();
}

Link linkFrom(const Json& j) {
    return Link{j.at("id").get<std::string>(), j.at("secretCiphertext").get<std::string>(), j.at("secretNonce").get<std::string>(),
                j.at("approval").get<bool>(), j.value("createdAt", std::string())};
}

Link linkIn(const Json& body) {
    return parse(body, [](const Json& j) { return linkFrom(j.at("link")); });
}

std::string groupPath(const std::string& conversationId, const char* rest) {
    return "group-invites/groups/" + percentEncode(conversationId) + rest;
}

std::string linkPath(const std::string& linkId, const char* action) {
    return "group-invites/links/" + percentEncode(linkId) + action;
}

Json nullable(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

}  // namespace

std::optional<Link> getLink(ApiClient& api, const std::string& conversationId) {
    return parse(api.call("GET", groupPath(conversationId, "/link")), [](const Json& j) -> std::optional<Link> {
        const Json& link = j.at("link");
        if (link.is_null()) return std::nullopt;
        return linkFrom(link);
    });
}

Link setLink(ApiClient& api, const std::string& conversationId, const crypto::invites::LinkInput& input) {
    Json body{{"auth", input.auth},
              {"secretCiphertext", input.secretCiphertext},
              {"secretNonce", input.secretNonce},
              {"previewCiphertext", input.previewCiphertext},
              {"previewNonce", input.previewNonce},
              {"wrappedKey", nullable(input.wrappedKey)},
              {"wrappedKeyNonce", nullable(input.wrappedKeyNonce)}};
    return linkIn(api.call("PUT", groupPath(conversationId, "/link"), body));
}

Link setApproval(ApiClient& api, const std::string& conversationId, bool approval, const std::optional<crypto::invites::WrappedKey>& wrapped) {
    Json body{{"approval", approval}};
    if (wrapped) {
        body["wrappedKey"] = wrapped->wrappedKey;
        body["wrappedKeyNonce"] = wrapped->wrappedKeyNonce;
    }
    return linkIn(api.call("PATCH", groupPath(conversationId, "/link"), body));
}

void deleteLink(ApiClient& api, const std::string& conversationId) {
    api.call("DELETE", groupPath(conversationId, "/link"));
}

std::vector<JoinRequest> listRequests(ApiClient& api, const std::string& conversationId) {
    return parse(api.call("GET", groupPath(conversationId, "/requests")), [](const Json& list) {
        std::vector<JoinRequest> out;
        for (const auto& r : list) out.push_back(JoinRequest{r.at("userId").get<std::string>(), r.value("createdAt", std::string())});
        return out;
    });
}

void approve(ApiClient& api, const std::string& conversationId, const std::string& userId, const std::string& wrappedKey,
             const std::optional<int>& keyEpoch, const std::optional<KeyNoticeDto>& keyNotice) {
    Json body{{"wrappedKey", wrappedKey}};
    if (keyEpoch) body["keyEpoch"] = *keyEpoch;
    if (keyNotice) body["keyNotice"] = *keyNotice;
    api.call("POST", groupPath(conversationId, "/requests/") + percentEncode(userId) + "/approve", body);
}

void deny(ApiClient& api, const std::string& conversationId, const std::string& userId) {
    api.call("DELETE", groupPath(conversationId, "/requests/") + percentEncode(userId));
}

Preview open(ApiClient& api, const std::string& linkId, const std::string& auth) {
    return parse(api.call("POST", linkPath(linkId, "/open"), Json{{"auth", auth}}), [](const Json& j) {
        Preview p;
        p.conversationId = j.at("conversationId").get<std::string>();
        p.previewCiphertext = j.at("previewCiphertext").get<std::string>();
        p.previewNonce = j.at("previewNonce").get<std::string>();
        p.memberCount = j.at("memberCount").get<int>();
        p.approval = j.at("approval").get<bool>();
        p.wrappedKey = optionalText(j, "wrappedKey");
        p.wrappedKeyNonce = optionalText(j, "wrappedKeyNonce");
        p.member = j.value("member", false);
        p.requested = j.value("requested", false);
        return p;
    });
}

JoinResult join(ApiClient& api, const std::string& linkId, const std::string& auth, const std::optional<std::string>& wrappedKey) {
    return parse(api.call("POST", linkPath(linkId, "/join"), Json{{"auth", auth}, {"wrappedKey", nullable(wrappedKey)}}), [](const Json& j) {
        return JoinResult{j.at("status").get<std::string>(), j.at("conversationId").get<std::string>()};
    });
}

}  // namespace redvault::api::invites

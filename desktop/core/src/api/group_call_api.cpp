#include "redvault/api/group_call_api.hpp"

#include <nlohmann/json.hpp>

namespace redvault::api::groupcalls {

namespace {

template <typename T>
T field(const Json& j, const char* key) {
    return j.at(key).get<T>();
}

template <typename F>
auto parse(const Json& j, F&& f) -> decltype(f(j)) {
    try {
        return f(j);
    } catch (const Json::exception& e) {
        throw ApiError(0, "malformed_response", std::string("Unexpected response: ") + e.what());
    }
}

Join joinFrom(const Json& j) {
    Join out;
    out.callId = field<std::string>(j, "callId");
    out.conversationId = field<std::string>(j, "conversationId");
    out.kind = field<std::string>(j, "kind");
    out.url = field<std::string>(j, "url");
    out.token = field<std::string>(j, "token");
    out.identity = field<std::string>(j, "identity");
    const auto created = j.find("created");
    out.created = created != j.end() && created->is_boolean() && created->get<bool>();
    return out;
}

std::string callPath(const std::string& callId, const char* action) {
    return "group-calls/" + live::urlEncode(callId) + "/" + action;
}

}  // namespace

Join start(ApiClient& api, const std::string& conversationId, const std::string& kind) {
    return parse(api.call("POST", "group-calls", Json{{"conversationId", conversationId}, {"kind", kind}}), joinFrom);
}

Join join(ApiClient& api, const std::string& callId) {
    return parse(api.call("POST", callPath(callId, "join")), joinFrom);
}

std::vector<ActiveCall> active(ApiClient& api) {
    return parse(api.call("GET", "group-calls/active"), [](const Json& body) {
        std::vector<ActiveCall> out;
        for (const auto& c : body.at("calls")) {
            out.push_back(ActiveCall{field<std::string>(c, "callId"), field<std::string>(c, "conversationId"), field<std::string>(c, "kind"),
                                     field<int>(c, "participants")});
        }
        return out;
    });
}

void decline(ApiClient& api, const std::string& callId) {
    api.call("POST", callPath(callId, "decline"));
}

void leave(ApiClient& api, const std::string& callId) {
    api.call("POST", callPath(callId, "leave"));
}

void sendKeys(ApiClient& api, const std::string& callId, std::int64_t epoch, const std::vector<SealedKey>& keys) {
    Json list = Json::array();
    for (const auto& k : keys) {
        list.push_back(Json{{"userId", k.userId}, {"key", Json{{"ciphertext", k.key.ciphertext}, {"nonce", k.key.nonce}}}});
    }
    api.call("POST", callPath(callId, "keys"), Json{{"epoch", epoch}, {"keys", list}});
}

std::string signalingUrl(const std::string& url, const std::string& siteBase) {
    if (url.empty()) return {};
    if (url.front() != '/') return url;  // already absolute (wss://...)
    // scheme://authority of the site.
    const auto schemeEnd = siteBase.find("://");
    if (schemeEnd == std::string::npos) return {};
    const std::string scheme = siteBase.substr(0, schemeEnd);
    const auto authorityEnd = siteBase.find('/', schemeEnd + 3);
    const std::string authority = siteBase.substr(schemeEnd + 3, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - schemeEnd - 3);
    if (authority.empty()) return {};
    return std::string(scheme == "https" ? "wss" : "ws") + "://" + authority + url;
}

}  // namespace redvault::api::groupcalls

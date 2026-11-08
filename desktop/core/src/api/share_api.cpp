// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/api/share_api.hpp"

#include <nlohmann/json.hpp>

#include <cctype>

#include "redvault/api/api_error.hpp"

namespace redvault::api::shares {

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

template <typename F>
auto parse(const Json& j, F&& f) -> decltype(f(j)) {
    try {
        return f(j);
    } catch (const Json::exception& e) {
        throw ApiError(0, "malformed_response", std::string("Unexpected response: ") + e.what());
    }
}

std::string encode(const std::string& value) {
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            static const char* hex = "0123456789ABCDEF";
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

Link linkFrom(const Json& j) {
    Link l;
    l.id = field<std::string>(j, "id");
    l.publicId = field<std::string>(j, "publicId");
    l.backupId = field<std::string>(j, "backupId");
    l.kind = field<std::string>(j, "kind");
    l.permission = field<std::string>(j, "permission");
    l.keyEpoch = field<int>(j, "keyEpoch");
    if (const auto it = j.find("password"); it != j.end() && it->is_object()) {
        l.password = Password{field<std::string>(*it, "salt"), field<unsigned long long>(*it, "ops"), field<std::size_t>(*it, "mem")};
    }
    l.envelope = field<std::string>(j, "envelope");
    l.envelopeNonce = field<std::string>(j, "envelopeNonce");
    l.ownerBox = field<std::string>(j, "ownerBox");
    l.ownerBoxNonce = field<std::string>(j, "ownerBoxNonce");
    l.uploadKeySalt = field<std::string>(j, "uploadKeySalt");
    l.expiresAt = optField<std::string>(j, "expiresAt");
    l.expired = optField<bool>(j, "expired").value_or(false);
    l.revoked = field<bool>(j, "revoked");
    l.maxDownloads = optField<std::int64_t>(j, "maxDownloads");
    l.downloadCount = field<std::int64_t>(j, "downloadCount");
    l.openCount = optField<std::int64_t>(j, "openCount").value_or(0);
    if (const auto it = j.find("upload"); it != j.end() && it->is_object()) {
        l.upload.maxFileSize = optField<std::int64_t>(*it, "maxFileSize");
        l.upload.allowance = optField<std::int64_t>(*it, "allowance");
        l.upload.maxFiles = optField<std::int64_t>(*it, "maxFiles");
        l.upload.count = optField<std::int64_t>(*it, "count").value_or(0);
        l.upload.bytes = optField<std::int64_t>(*it, "bytes").value_or(0);
        l.upload.inProgress = optField<std::int64_t>(*it, "inProgress").value_or(0);
    }
    l.incoming = optField<int>(j, "incoming").value_or(0);
    return l;
}

std::string path(const std::string& id, const char* suffix = "") {
    return "shares/" + encode(id) + suffix;
}

}  // namespace

LinkList list(ApiClient& api) {
    return parse(api.call("GET", "shares"), [](const Json& j) {
        LinkList out;
        for (const auto& l : j.at("links")) out.links.push_back(linkFrom(l));
        out.incoming = optField<int>(j, "incoming").value_or(0);
        return out;
    });
}

Link create(ApiClient& api, const Json& body) {
    return parse(api.call("POST", "shares", std::optional<Json>(body)), linkFrom);
}

Link update(ApiClient& api, const std::string& id, const Json& body) {
    return parse(api.call("PATCH", path(id), std::optional<Json>(body)), linkFrom);
}

Link setPassword(ApiClient& api, const std::string& id, const Json& body) {
    return parse(api.call("PUT", path(id, "/password"), std::optional<Json>(body)), linkFrom);
}

Link regenerate(ApiClient& api, const std::string& id, const Json& body) {
    return parse(api.call("POST", path(id, "/regenerate"), std::optional<Json>(body)), linkFrom);
}

Link revoke(ApiClient& api, const std::string& id) {
    return parse(api.call("POST", path(id, "/revoke"), Json::object()), linkFrom);
}

void remove(ApiClient& api, const std::string& id) {
    api.call("DELETE", path(id));
}

Items items(ApiClient& api, const std::string& id) {
    return parse(api.call("GET", path(id, "/items")), [](const Json& j) {
        Items out;
        out.keyEpoch = field<int>(j, "keyEpoch");
        for (const auto& i : j.at("items")) {
            out.items.push_back(OwnerItem{field<std::string>(i, "itemId"), field<std::string>(i, "backupId"), field<std::string>(i, "kind"),
                                          field<std::string>(i, "metaCiphertext"), field<std::string>(i, "metaNonce")});
        }
        return out;
    });
}

int upsertItems(ApiClient& api, const std::string& id, int keyEpoch, const std::vector<ItemInput>& items) {
    Json list = Json::array();
    for (const auto& i : items) {
        list.push_back({{"itemId", i.itemId}, {"backupId", i.backupId}, {"metaCiphertext", i.metaCiphertext}, {"metaNonce", i.metaNonce}});
    }
    return parse(api.call("POST", path(id, "/items"), Json{{"keyEpoch", keyEpoch}, {"items", list}}),
                 [](const Json& j) { return field<int>(j, "stored"); });
}

int removeItems(ApiClient& api, const std::string& id, const std::vector<std::string>& itemIds) {
    return parse(api.call("POST", path(id, "/items/remove"), Json{{"itemIds", itemIds}}), [](const Json& j) { return field<int>(j, "removed"); });
}

std::vector<Incoming> incoming(ApiClient& api) {
    return parse(api.call("GET", "shares/incoming"), [](const Json& j) {
        std::vector<Incoming> out;
        for (const auto& i : j.at("items")) {
            out.push_back(Incoming{field<std::string>(i, "id"), optField<std::string>(i, "shareId"), field<std::int64_t>(i, "encryptedSize"),
                                   optField<std::string>(i, "ciphertextSha256").value_or(""), optField<std::string>(i, "sealedMeta").value_or(""),
                                   field<std::string>(i, "keySalt")});
        }
        return out;
    });
}

drive::Entry accept(ApiClient& api, const std::string& id, const std::string& metaCiphertext, const std::string& metaNonce) {
    return parse(api.call("POST", "shares/incoming/" + encode(id) + "/accept", Json{{"metaCiphertext", metaCiphertext}, {"metaNonce", metaNonce}}),
                 [](const Json& j) {
                     const auto& item = j.at("item");
                     drive::Entry e;
                     e.id = field<std::string>(item, "id");
                     e.kind = optField<std::string>(item, "kind").value_or("file");
                     e.encryptedSize = field<std::int64_t>(item, "encryptedSize");
                     e.ciphertextSha256 = optField<std::string>(item, "ciphertextSha256").value_or("");
                     e.metaCiphertext = field<std::string>(item, "metaCiphertext");
                     e.metaNonce = field<std::string>(item, "metaNonce");
                     return e;
                 });
}

void reject(ApiClient& api, const std::string& id) {
    api.call("POST", "shares/incoming/" + encode(id) + "/reject", Json::object());
}

}  // namespace redvault::api::shares

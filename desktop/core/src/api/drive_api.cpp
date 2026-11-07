#include "redvault/api/drive_api.hpp"

#include <nlohmann/json.hpp>

#include <cctype>

#include "redvault/api/api_error.hpp"

namespace redvault::api {

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

}  // namespace

namespace drive {

namespace {

Usage usageFrom(const Json& j) {
    return Usage{field<std::int64_t>(j, "used"), field<std::int64_t>(j, "limit"), field<std::int64_t>(j, "remaining"),
                 field<std::string>(j, "tier")};
}

Entry entryFrom(const Json& j) {
    Entry e;
    e.id = field<std::string>(j, "id");
    e.kind = optField<std::string>(j, "kind").value_or("file");
    e.encryptedSize = field<std::int64_t>(j, "encryptedSize");
    e.ciphertextSha256 = optField<std::string>(j, "ciphertextSha256").value_or("");
    e.metaCiphertext = field<std::string>(j, "metaCiphertext");
    e.metaNonce = field<std::string>(j, "metaNonce");
    e.sharedWith = optField<std::vector<std::string>>(j, "sharedWith").value_or(std::vector<std::string>{});
    return e;
}

Link linkFrom(const Json& j) {
    Link l;
    l.id = field<std::string>(j, "id");
    l.backupId = field<std::string>(j, "backupId");
    l.sealedMeta = field<std::string>(j, "sealedMeta");
    l.metaNonce = field<std::string>(j, "metaNonce");
    l.hasPassword = field<bool>(j, "hasPassword");
    l.allowDownload = field<bool>(j, "allowDownload");
    l.expiresAt = optField<std::string>(j, "expiresAt");
    return l;
}

}  // namespace

Usage usage(ApiClient& api) {
    return parse(api.call("GET", "backup/usage"), usageFrom);
}

Listing list(ApiClient& api) {
    return parse(api.call("GET", "backup/items"), [](const Json& j) {
        Listing out;
        for (const auto& item : j.at("items")) out.items.push_back(entryFrom(item));
        out.usage = usageFrom(j.at("usage"));
        return out;
    });
}

std::vector<SharedEntry> sharedWithMe(ApiClient& api) {
    return parse(api.call("GET", "backup/shared-with-me"), [](const Json& j) {
        std::vector<SharedEntry> out;
        for (const auto& item : j.at("items")) {
            out.push_back(SharedEntry{field<std::string>(item, "id"), field<std::string>(item, "ownerId"),
                                      field<std::int64_t>(item, "encryptedSize"),
                                      optField<std::string>(item, "ciphertextSha256").value_or(""), field<std::string>(item, "sealedMeta")});
        }
        return out;
    });
}

Entry createFolder(ApiClient& api, const std::string& metaCiphertext, const std::string& metaNonce) {
    return parse(api.call("POST", "backup/folders", Json{{"metaCiphertext", metaCiphertext}, {"metaNonce", metaNonce}}), entryFrom);
}

Entry updateMeta(ApiClient& api, const std::string& id, const std::string& metaCiphertext, const std::string& metaNonce) {
    return parse(api.call("PUT", "backup/" + encode(id) + "/meta", Json{{"metaCiphertext", metaCiphertext}, {"metaNonce", metaNonce}}),
                 entryFrom);
}

UploadAuth requestUpload(ApiClient& api, std::int64_t encryptedSize, const std::string& ciphertextSha256Hex, const std::string& metaCiphertext,
                         const std::string& metaNonce) {
    Json body{{"encryptedSize", encryptedSize},
              {"ciphertextSha256", ciphertextSha256Hex},
              {"metaCiphertext", metaCiphertext},
              {"metaNonce", metaNonce}};
    return parse(api.call("POST", "backup/upload-request", body), [](const Json& j) {
        return UploadAuth{field<std::string>(j, "objectId"), j.at("upload").get<TransferTargetDto>()};
    });
}

UploadAuth requestUploadParts(ApiClient& api, std::int64_t encryptedSize, const std::string& ciphertextSha256Hex,
                              const std::string& metaCiphertext, const std::string& metaNonce, std::uint64_t partSize,
                              const std::string& partsSha256Hex) {
    Json body{{"encryptedSize", encryptedSize},
              {"ciphertextSha256", ciphertextSha256Hex},
              {"metaCiphertext", metaCiphertext},
              {"metaNonce", metaNonce},
              {"partSize", partSize},
              {"partsSha256", partsSha256Hex}};
    return parse(api.call("POST", "backup/upload-request", body), [](const Json& j) {
        const Json& upload = j.at("upload");
        UploadAuth auth{field<std::string>(j, "objectId"), upload.get<TransferTargetDto>()};
        auth.partSize = upload.value("partSize", std::uint64_t{0});
        auth.partCount = upload.value("partCount", 0);
        return auth;
    });
}

std::vector<ReceivedPart> listParts(ApiClient& api, const std::string& id) {
    return parse(api.call("GET", "backup/" + encode(id) + "/parts"), [](const Json& j) {
        std::vector<ReceivedPart> parts;
        for (const auto& p : j.at("received")) {
            parts.push_back({p.at("partNumber").get<int>(), p.at("size").get<std::uint64_t>(), p.at("sha256").get<std::string>()});
        }
        return parts;
    });
}

void completeUpload(ApiClient& api, const std::string& id) {
    api.call("POST", "backup/" + encode(id) + "/complete", Json::object());
}

TransferTargetDto download(ApiClient& api, const std::string& id) {
    return parse(api.call("GET", "backup/" + encode(id) + "/download"), [](const Json& j) { return j.at("download").get<TransferTargetDto>(); });
}

void remove(ApiClient& api, const std::string& id) {
    api.call("DELETE", "backup/" + encode(id));
}

void share(ApiClient& api, const std::string& id, const std::string& recipientId, const std::string& sealedMeta) {
    api.call("POST", "backup/" + encode(id) + "/shares", Json{{"recipientId", recipientId}, {"sealedMeta", sealedMeta}});
}

void unshare(ApiClient& api, const std::string& id, const std::string& recipientId) {
    api.call("DELETE", "backup/" + encode(id) + "/shares/" + encode(recipientId));
}

Link createLink(ApiClient& api, const std::string& id, const std::string& sealedMeta, const std::string& metaNonce,
                const std::optional<std::string>& passwordSalt, bool allowDownload, std::optional<int> expiresInDays) {
    Json body{{"sealedMeta", sealedMeta},
              {"metaNonce", metaNonce},
              {"passwordSalt", passwordSalt ? Json(*passwordSalt) : Json(nullptr)},
              {"allowDownload", allowDownload},
              {"expiresInDays", expiresInDays ? Json(*expiresInDays) : Json(nullptr)}};
    return parse(api.call("POST", "backup/" + encode(id) + "/links", body), linkFrom);
}

std::vector<Link> links(ApiClient& api) {
    return parse(api.call("GET", "backup/links"), [](const Json& j) {
        std::vector<Link> out;
        for (const auto& l : j.at("links")) out.push_back(linkFrom(l));
        return out;
    });
}

void deleteLink(ApiClient& api, const std::string& linkId) {
    api.call("DELETE", "backup/links/" + encode(linkId));
}

}  // namespace drive

namespace vault {

namespace {

Item itemFrom(const Json& j) {
    return Item{field<std::string>(j, "id"), field<std::string>(j, "ciphertext"), field<std::string>(j, "nonce"), field<int>(j, "revision")};
}

}  // namespace

std::vector<Item> list(ApiClient& api) {
    return parse(api.call("GET", "vault/items"), [](const Json& j) {
        std::vector<Item> out;
        for (const auto& item : j.at("items")) out.push_back(itemFrom(item));
        return out;
    });
}

Item create(ApiClient& api, const std::string& ciphertext, const std::string& nonce) {
    return parse(api.call("POST", "vault/items", Json{{"ciphertext", ciphertext}, {"nonce", nonce}}), itemFrom);
}

Item update(ApiClient& api, const std::string& id, int revision, const std::string& ciphertext, const std::string& nonce) {
    return parse(api.call("PUT", "vault/items/" + encode(id), Json{{"ciphertext", ciphertext}, {"nonce", nonce}, {"revision", revision}}),
                 itemFrom);
}

void remove(ApiClient& api, const std::string& id) {
    api.call("DELETE", "vault/items/" + encode(id));
}

std::vector<Share> shares(ApiClient& api) {
    return parse(api.call("GET", "vault/shares"), [](const Json& j) {
        std::vector<Share> out;
        for (const auto& s : j.at("shares")) out.push_back(Share{field<std::string>(s, "itemId"), field<std::string>(s, "recipientId")});
        return out;
    });
}

std::vector<SharedItem> sharedWithMe(ApiClient& api) {
    return parse(api.call("GET", "vault/shared-with-me"), [](const Json& j) {
        std::vector<SharedItem> out;
        for (const auto& s : j.at("items")) {
            out.push_back(SharedItem{field<std::string>(s, "itemId"), field<std::string>(s, "ownerId"), field<std::string>(s, "sealed")});
        }
        return out;
    });
}

void share(ApiClient& api, const std::string& itemId, const std::string& recipientId, const std::string& sealed) {
    api.call("PUT", "vault/items/" + encode(itemId) + "/shares/" + encode(recipientId), Json{{"sealed", sealed}});
}

void unshare(ApiClient& api, const std::string& itemId, const std::string& recipientId) {
    api.call("DELETE", "vault/items/" + encode(itemId) + "/shares/" + encode(recipientId));
}

std::string breachRange(ApiClient& api, const std::string& prefix) {
    return api.callText("GET", "vault/breach-range/" + encode(prefix));
}

}  // namespace vault

}  // namespace redvault::api

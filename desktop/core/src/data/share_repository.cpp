// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/data/share_repository.hpp"

#include <algorithm>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <thread>

#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/call_crypto.hpp"
#include "redvault/crypto/drive_crypto.hpp"
#include "redvault/crypto/file_crypto.hpp"
#include "redvault/data/transfer.hpp"
#include "redvault/model/file_message.hpp"
#include "redvault/net/http.hpp"
#include "redvault/util/text.hpp"

namespace redvault::data {

namespace fs = std::filesystem;
using api::ApiError;
using api::Json;
using api::UserFacingError;
namespace share = crypto::share;

namespace {

constexpr std::size_t kBatch = 200;

template <typename F>
auto mapErrors(const std::string& fallback, F&& block) -> decltype(block()) {
    try {
        return block();
    } catch (const UserFacingError&) {
        throw;
    } catch (const crypto::files::FileDecryptError&) {
        throw UserFacingError("Unable to decrypt this file. It may be corrupted or tampered with.");
    } catch (const ApiError& e) {
        if (e.status() == 401) throw;
        const std::string& code = e.code();
        // The server's own wording for the link rules is written to be shown.
        if (code.starts_with("share_") || code == "validation_error" || code == "backup_storage_full" || code == "rate_limited") {
            throw UserFacingError(e.what());
        }
        if (code == "backup_disabled") throw UserFacingError("Files aren't available on this server.");
        if (code == "network_error") throw UserFacingError("Network interrupted. Try again.");
        if (e.status() == 404) throw UserFacingError("That isn't there anymore. Refresh and try again.");
        throw UserFacingError(fallback);
    } catch (const net::NetworkError&) {
        throw UserFacingError("Network interrupted. Try again.");
    } catch (const crypto::CryptoError&) {
        throw UserFacingError(fallback);
    } catch (const Json::exception&) {
        throw UserFacingError(fallback);
    } catch (const std::ios_base::failure&) {
        throw UserFacingError(fallback);
    } catch (const fs::filesystem_error&) {
        throw UserFacingError(fallback);
    }
}

void removeQuietly(const fs::path& path) {
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

/// The Files metadata of a file, as the share link item sees it; nullopt for a locked one (its key isn't in the metadata).
std::optional<Json> fileMeta(const DriveEntry& e, const std::string& parent) {
    if (!e.meta.is_object() || !e.meta.contains("key") || !e.meta.contains("header") || !e.meta.contains("sha")) return std::nullopt;
    if (!e.meta.at("key").is_string() || !e.meta.at("header").is_string() || !e.meta.at("sha").is_string()) return std::nullopt;
    return Json{{"v", 1},
                {"kind", "file"},
                {"name", e.name},
                {"parent", parent},
                {"mime", e.mimeType},
                {"size", e.size},
                {"modified", std::max<std::int64_t>(0, e.modifiedMs)},
                {"key", e.meta.at("key")},
                {"header", e.meta.at("header")},
                {"chunk", e.meta.value("chunk", static_cast<int>(crypto::files::kChunkBytes))},
                {"sha", e.meta.at("sha")}};
}

bool newer(const DriveEntry& a, const DriveEntry& b) {
    return a.version > b.version || (a.version == b.version && a.modifiedMs > b.modifiedMs);
}

}  // namespace

ShareRepository::ShareRepository(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const SelfIdentity> identity, fs::path cacheDir,
                                 std::string webAppBase, Sleep sleep)
    : api_(std::move(api)),
      identity_(std::move(identity)),
      cacheDir_(std::move(cacheDir)),
      webAppBase_(std::move(webAppBase)),
      sleep_(sleep ? std::move(sleep) : Sleep([](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); })) {}

OwnerShare ShareRepository::decode(const api::shares::Link& link) const {
    const auto driveKey = crypto::drive::driveKey(identity_->privateKey.view());
    OwnerShare out;
    out.link = link;
    out.status = link.revoked ? "revoked" : link.expired ? "expired" : "active";
    try {
        out.box = share::openOwnerBox(driveKey.view(), crypto::base64Decode(link.ownerBox), crypto::base64Decode(link.ownerBoxNonce));
        if (out.box) {
            if (const auto secret = crypto::calls::linkSecretFromFragment(out.box->secret)) {
                out.url = share::shareUrl(webAppBase_, link.publicId, secret->view());
            }
        }
    } catch (const std::exception&) {
        out.box.reset();
    }
    return out;
}

IncomingFile ShareRepository::decodeIncoming(const api::shares::Incoming& dto) const {
    IncomingFile out;
    out.dto = dto;
    try {
        const auto driveKey = crypto::drive::driveKey(identity_->privateKey.view());
        const auto pair = share::uploadKeyPair(driveKey.view(), crypto::base64Decode(dto.keySalt));
        out.meta = share::openContribution(pair, crypto::base64Decode(dto.sealedMeta));
        // The server's hash and the sealed one must agree: nothing else can be swapped in.
        if (out.meta && out.meta->at("sha").get<std::string>() != dto.ciphertextSha256) out.meta.reset();
    } catch (const std::exception&) {
        out.meta.reset();
    }
    std::string full = out.meta ? out.meta->at("name").get<std::string>() : std::string();
    const auto slash = full.find_last_of('/');
    // An anonymous uploader chose these: no control, bidi or zero-width characters, no trailing dots, no reserved names.
    out.name = model::file_message::sanitizeFilename(slash == std::string::npos ? full : full.substr(slash + 1));
    if (slash != std::string::npos) {
        for (std::size_t start = 0; start <= slash;) {
            const auto end = std::min(full.find('/', start), slash);
            const auto part = model::file_message::sanitizeFilename(full.substr(start, end - start));
            if (!part.empty()) out.folder += (out.folder.empty() ? "" : "/") + part;
            start = end + 1;
        }
    }
    if (out.name.empty()) out.name = "Encrypted file";
    return out;
}

ShareRepository::Listing ShareRepository::refresh() {
    return mapErrors("Couldn't load your links. Try again.", [&] {
        Listing out;
        for (const auto& link : api::shares::list(*api_).links) out.links.push_back(decode(link));
        for (const auto& i : api::shares::incoming(*api_)) out.incoming.push_back(decodeIncoming(i));
        return out;
    });
}

Json ShareRepository::passwordJson(const std::optional<share::PasswordParams>& params) const {
    if (!params) return nullptr;
    return Json{{"salt", crypto::base64Encode(params->salt)}, {"ops", params->ops}, {"mem", params->mem}};
}

std::vector<ShareItemPlan> ShareRepository::plan(const std::string& rootBackupId, const std::string& rootItemId,
                                                 const std::vector<DriveEntry>& entries, const std::vector<api::shares::OwnerItem>& existing) {
    std::map<std::string, const DriveEntry*> byId;
    for (const auto& e : entries) byId[e.id] = &e;
    const auto rootIt = byId.find(rootBackupId);
    if (rootIt == byId.end() || rootIt->second->trashed) return {};
    const DriveEntry& root = *rootIt->second;

    std::map<std::string, std::string> idByBackup;
    std::map<std::string, std::string> idByFileId;
    for (const auto& i : existing) {
        idByBackup[i.backupId] = i.itemId;
        if (const auto it = byId.find(i.backupId); it != byId.end() && it->second->kind == "file") idByFileId[it->second->fileId] = i.itemId;
    }

    if (root.kind == "file") {
        const auto meta = fileMeta(root, "");
        if (!meta) return {};
        return {ShareItemPlan{rootItemId, root.id, *meta}};
    }

    std::vector<ShareItemPlan> out{ShareItemPlan{rootItemId, root.id, Json{{"v", 1}, {"kind", "folder"}, {"name", root.name}, {"parent", ""}}}};
    std::map<std::string, std::vector<const DriveEntry*>> children;
    for (const auto& e : entries) {
        if (!e.trashed && !e.incoming) children[e.parent].push_back(&e);
    }
    std::set<std::string> visited{root.id};
    struct Step {
        const DriveEntry* folder;
        std::string itemId;
        int depth;
    };
    std::deque<Step> queue{Step{&root, rootItemId, 0}};
    while (!queue.empty()) {
        const Step step = queue.front();
        queue.pop_front();
        std::map<std::string, const DriveEntry*> newest;
        for (const DriveEntry* e : children[step.folder->id]) {
            if (e->kind == "folder") {
                if (visited.contains(e->id) || step.depth >= 64) continue;
                visited.insert(e->id);
                const auto known = idByBackup.find(e->id);
                const std::string childId = known != idByBackup.end() ? known->second : util::randomUuid();
                out.push_back(ShareItemPlan{childId, e->id, Json{{"v", 1}, {"kind", "folder"}, {"name", e->name}, {"parent", step.itemId}}});
                queue.push_back(Step{e, childId, step.depth + 1});
            } else {
                auto& slot = newest[e->fileId];
                if (!slot || newer(*e, *slot)) slot = e;
            }
        }
        for (const auto& [fileId, e] : newest) {
            const auto meta = fileMeta(*e, step.itemId);
            if (!meta) continue;
            std::string itemId;
            if (const auto f = idByFileId.find(fileId); f != idByFileId.end()) itemId = f->second;
            else if (const auto b = idByBackup.find(e->id); b != idByBackup.end()) itemId = b->second;
            else itemId = util::randomUuid();
            out.push_back(ShareItemPlan{itemId, e->id, *meta});
        }
        if (out.size() > kMaxShareItems + 1) break;
    }
    return out;
}

OwnerShare ShareRepository::create(const DriveEntry& root, const std::vector<DriveEntry>& entries, const ShareOptions& options) {
    if (root.locked()) throw UserFacingError("This file is locked with its own key or password. Unlock it first, then share it.");
    if (options.permission == "contributor" && root.kind != "folder") throw UserFacingError("Only folders can take uploads.");
    return mapErrors("Couldn't make the link. Try again.", [&] {
        const auto driveKey = crypto::drive::driveKey(identity_->privateKey.view());
        const auto secret = share::newLinkSecret();
        const auto shareKey = share::newShareKey();
        const auto uploadSalt = share::newSalt();
        std::optional<share::PasswordParams> params;
        if (!options.password.empty()) params = share::newPasswordParams();
        const auto keys = share::deriveKeys(secret.view(), options.password.empty() ? std::nullopt : std::optional<std::string_view>(options.password),
                                            params);
        const std::string rootItemId = util::randomUuid();
        if (plan(root.id, rootItemId, entries, {}).size() > kMaxShareItems) {
            throw UserFacingError("A link can show up to 5,000 items. Share a smaller folder.");
        }
        const auto envelope =
            share::sealEnvelope(keys.wrap.view(), share::Envelope{shareKey, share::uploadKeyPair(driveKey.view(), uploadSalt).publicKey, rootItemId});
        const share::OwnerBox box{crypto::calls::linkSecretToFragment(secret.view()), crypto::base64Encode(shareKey), rootItemId,
                                  options.label.empty() ? root.name : options.label, util::nowMillis()};
        const auto owner = share::sealOwnerBox(driveKey.view(), box);
        Json body{{"backupId", root.id},
                  {"permission", options.permission},
                  {"lookupVerifier", share::verifierOf(keys.lookup.view())},
                  {"accessVerifier", share::verifierOf(keys.access.view())},
                  {"password", passwordJson(params)},
                  {"envelope", crypto::base64Encode(envelope.ciphertext)},
                  {"envelopeNonce", crypto::base64Encode(envelope.nonce)},
                  {"ownerBox", crypto::base64Encode(owner.ciphertext)},
                  {"ownerBoxNonce", crypto::base64Encode(owner.nonce)},
                  {"uploadKeySalt", crypto::base64Encode(uploadSalt)},
                  {"expiresAt", options.expiresAtMs ? Json(util::formatIsoMillis(*options.expiresAtMs)) : Json(nullptr)},
                  {"maxDownloads", options.maxDownloads ? Json(*options.maxDownloads) : Json(nullptr)}};
        if (options.permission == "contributor") {
            const auto opt = [](const std::optional<std::int64_t>& v) { return v ? Json(*v) : Json(nullptr); };
            body["upload"] = Json{{"maxFileSize", opt(options.maxFileSize)}, {"allowance", opt(options.allowance)}, {"maxFiles", opt(options.maxFiles)}};
        } else {
            body["upload"] = nullptr;
        }
        const auto made = decode(api::shares::create(*api_, body));
        reconcile(made, entries, std::vector<api::shares::OwnerItem>{});
        return made;
    });
}

ReconcileResult ShareRepository::reconcile(const OwnerShare& s, const std::vector<DriveEntry>& entries,
                                           const std::optional<std::vector<api::shares::OwnerItem>>& known) {
    ReconcileResult result;
    if (!s.box || s.status != "active") return result;
    return mapErrors("Couldn't update the link. Try again.", [&] {
        const auto shareKey = crypto::base64Decode(s.box->key);
        const auto fetched = known ? *known : api::shares::items(*api_, s.link.id).items;
        const auto desired = plan(s.link.backupId, s.box->root, entries, fetched);
        if (desired.size() > kMaxShareItems) throw UserFacingError("A link can show up to 5,000 items. Share a smaller folder.");
        std::map<std::string, const api::shares::OwnerItem*> current;
        for (const auto& i : fetched) current[i.itemId] = &i;
        std::set<std::string> wanted;
        std::vector<api::shares::ItemInput> upserts;
        for (const auto& d : desired) {
            wanted.insert(d.itemId);
            const auto have = current.find(d.itemId);
            if (have != current.end() && have->second->backupId == d.backupId) {
                const auto meta = share::openItem(shareKey, have->second->itemId, crypto::base64Decode(have->second->metaCiphertext),
                                                  crypto::base64Decode(have->second->metaNonce));
                if (meta && *meta == d.meta) continue;
                ++result.updated;
            } else if (have != current.end()) {
                ++result.updated;
            } else {
                ++result.added;
            }
            const auto sealed = share::sealItem(shareKey, d.itemId, d.meta);
            upserts.push_back(api::shares::ItemInput{d.itemId, d.backupId, crypto::base64Encode(sealed.ciphertext), crypto::base64Encode(sealed.nonce)});
        }
        std::vector<std::string> removals;
        for (const auto& i : fetched) {
            if (!wanted.contains(i.itemId)) removals.push_back(i.itemId);
        }
        result.removed = static_cast<int>(removals.size());
        for (std::size_t i = 0; i < removals.size(); i += kBatch) {
            api::shares::removeItems(*api_, s.link.id, {removals.begin() + i, removals.begin() + std::min(removals.size(), i + kBatch)});
        }
        for (std::size_t i = 0; i < upserts.size(); i += kBatch) {
            api::shares::upsertItems(*api_, s.link.id, s.link.keyEpoch, {upserts.begin() + i, upserts.begin() + std::min(upserts.size(), i + kBatch)});
        }
        return result;
    });
}

void ShareRepository::reconcileAll(const std::vector<OwnerShare>& shares, const std::vector<DriveEntry>& entries) {
    for (const auto& s : shares) {
        if (s.status != "active" || !s.box) continue;
        try {
            reconcile(s, entries);
        } catch (const ApiError& e) {
            if (e.status() == 401) throw;
        } catch (const std::exception&) {
            // Tried again on the next refresh.
        }
    }
}

OwnerShare ShareRepository::updateSettings(const OwnerShare& s, const Json& settings, const std::optional<std::string>& label) {
    return mapErrors("Couldn't save the link. Try again.", [&] {
        Json body = settings.is_object() ? settings : Json::object();
        if (label && s.box && *label != s.box->label) {
            const auto driveKey = crypto::drive::driveKey(identity_->privateKey.view());
            auto box = *s.box;
            box.label = *label;
            const auto owner = share::sealOwnerBox(driveKey.view(), box);
            body["ownerBox"] = crypto::base64Encode(owner.ciphertext);
            body["ownerBoxNonce"] = crypto::base64Encode(owner.nonce);
        }
        return decode(api::shares::update(*api_, s.link.id, body));
    });
}

OwnerShare ShareRepository::setPassword(const OwnerShare& s, const std::string& password) {
    if (!s.box) throw UserFacingError("This link can't be changed on this device.");
    return mapErrors("Couldn't change the password. Try again.", [&] {
        const auto driveKey = crypto::drive::driveKey(identity_->privateKey.view());
        std::optional<share::PasswordParams> params;
        if (!password.empty()) params = share::newPasswordParams();
        const auto secret = crypto::calls::linkSecretFromFragment(s.box->secret);
        if (!secret) throw UserFacingError("This link can't be changed on this device.");
        const auto keys = share::deriveKeys(secret->view(), password.empty() ? std::nullopt : std::optional<std::string_view>(password), params);
        const auto envelope = share::sealEnvelope(
            keys.wrap.view(), share::Envelope{crypto::base64Decode(s.box->key),
                                              share::uploadKeyPair(driveKey.view(), crypto::base64Decode(s.link.uploadKeySalt)).publicKey, s.box->root});
        return decode(api::shares::setPassword(*api_, s.link.id,
                                               Json{{"password", passwordJson(params)},
                                                    {"accessVerifier", share::verifierOf(keys.access.view())},
                                                    {"envelope", crypto::base64Encode(envelope.ciphertext)},
                                                    {"envelopeNonce", crypto::base64Encode(envelope.nonce)}}));
    });
}

OwnerShare ShareRepository::regenerate(const OwnerShare& s, const std::string& password, const std::vector<DriveEntry>& entries) {
    if (!s.box) throw UserFacingError("This link can't be changed on this device.");
    return mapErrors("Couldn't make a new link. Try again.", [&] {
        const auto driveKey = crypto::drive::driveKey(identity_->privateKey.view());
        const auto secret = share::newLinkSecret();
        const auto shareKey = share::newShareKey();
        std::optional<share::PasswordParams> params;
        if (!password.empty()) params = share::newPasswordParams();
        const auto keys = share::deriveKeys(secret.view(), password.empty() ? std::nullopt : std::optional<std::string_view>(password), params);
        const std::string rootItemId = util::randomUuid();
        const auto envelope = share::sealEnvelope(
            keys.wrap.view(),
            share::Envelope{shareKey, share::uploadKeyPair(driveKey.view(), crypto::base64Decode(s.link.uploadKeySalt)).publicKey, rootItemId});
        auto box = *s.box;
        box.secret = crypto::calls::linkSecretToFragment(secret.view());
        box.key = crypto::base64Encode(shareKey);
        box.root = rootItemId;
        const auto owner = share::sealOwnerBox(driveKey.view(), box);
        const auto next = decode(api::shares::regenerate(*api_, s.link.id,
                                                         Json{{"lookupVerifier", share::verifierOf(keys.lookup.view())},
                                                              {"accessVerifier", share::verifierOf(keys.access.view())},
                                                              {"password", passwordJson(params)},
                                                              {"envelope", crypto::base64Encode(envelope.ciphertext)},
                                                              {"envelopeNonce", crypto::base64Encode(envelope.nonce)},
                                                              {"ownerBox", crypto::base64Encode(owner.ciphertext)},
                                                              {"ownerBoxNonce", crypto::base64Encode(owner.nonce)}}));
        reconcile(next, entries, std::vector<api::shares::OwnerItem>{});
        return next;
    });
}

OwnerShare ShareRepository::revoke(const OwnerShare& s) {
    return mapErrors("Couldn't revoke the link. Try again.", [&] { return decode(api::shares::revoke(*api_, s.link.id)); });
}

void ShareRepository::remove(const OwnerShare& s) {
    mapErrors("Couldn't delete the link. Try again.", [&] { api::shares::remove(*api_, s.link.id); });
}

void ShareRepository::downloadIncoming(const IncomingFile& file, const fs::path& destination, const ProgressFn& onProgress) {
    if (!file.meta) throw UserFacingError("This file can't be opened on this device.");
    mapErrors("Download failed. Try again.", [&] {
        api::TransferTargetDto target{"proxy", "/shares/incoming/" + file.dto.id + "/content", "GET", {}, util::formatIsoMillis(util::nowMillis() + 3600000)};
        const fs::path folder = cacheDir_ / "incoming-shares" / util::randomUuid();
        fs::create_directories(folder);
        struct Cleanup {
            fs::path path;
            ~Cleanup() { removeQuietly(path); }
        } cleanup{folder};
        const fs::path ciphertext = folder / ".ciphertext";
        const auto total = static_cast<std::uint64_t>(file.dto.encryptedSize);
        transferTarget(
            *api_, target, "GET", "/shares/incoming/",
            [&](const net::HttpRequest& request) {
                std::ofstream sink(ciphertext, std::ios::binary | std::ios::trunc);
                if (!sink) throw UserFacingError("Download failed. Try again.");
                auto response = api_->transport().sendStreaming(request, {.download = &sink});
                if (onProgress) onProgress(total, total);
                return response;
            },
            sleep_);
        const fs::path partial = destination.parent_path() / (destination.filename().u8string() + u8".part");
        try {
            std::ifstream input(ciphertext, std::ios::binary);
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            if (!input || !out) throw UserFacingError("Download failed. Try again.");
            crypto::files::decrypt(input, out, crypto::base64Decode(file.meta->at("key").get<std::string>()),
                                   crypto::base64Decode(file.meta->at("header").get<std::string>()), total, file.meta->at("sha").get<std::string>());
            out.flush();
            if (!out) throw UserFacingError("Download failed. Try again.");
            out.close();
            fs::rename(partial, destination);
        } catch (...) {
            removeQuietly(partial);
            throw;
        }
    });
}

DriveEntry ShareRepository::acceptIncoming(const IncomingFile& file, const std::string& parent) {
    if (!file.meta) throw UserFacingError("This file can't be opened on this device.");
    return mapErrors("Couldn't add the file. Try again.", [&] {
        const auto driveKey = crypto::drive::driveKey(identity_->privateKey.view());
        const Json meta{{"v", 1},
                        {"kind", "file"},
                        {"name", file.name},
                        {"parent", parent},
                        {"modified", util::nowMillis()},
                        {"trashed", false},
                        {"trashedAt", 0},
                        {"mime", file.meta->at("mime")},
                        {"size", file.meta->at("size")},
                        {"fileId", util::randomUuid()},
                        {"version", 1},
                        {"key", file.meta->at("key")},
                        {"header", file.meta->at("header")},
                        {"chunk", file.meta->at("chunk")},
                        {"sha", file.meta->at("sha")}};
        const auto sealed = crypto::drive::sealMeta(meta.dump(), driveKey.view());
        const auto entry = api::shares::accept(*api_, file.dto.id, crypto::base64Encode(sealed.ciphertext), crypto::base64Encode(sealed.nonce));
        DriveEntry out;
        out.id = entry.id;
        out.kind = "file";
        out.name = file.name;
        out.parent = parent;
        out.mimeType = meta.at("mime").get<std::string>();
        out.size = meta.at("size").get<std::int64_t>();
        out.encryptedSize = entry.encryptedSize;
        out.modifiedMs = meta.at("modified").get<std::int64_t>();
        out.fileId = meta.at("fileId").get<std::string>();
        out.meta = meta;
        return out;
    });
}

void ShareRepository::rejectIncoming(const IncomingFile& file) {
    mapErrors("Couldn't reject the file. Try again.", [&] { api::shares::reject(*api_, file.dto.id); });
}

}  // namespace redvault::data

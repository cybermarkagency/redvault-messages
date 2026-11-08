// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/data/story_drafts.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/file_crypto.hpp"
#include "redvault/crypto/story_crypto.hpp"
#include "redvault/util/private_files.hpp"
#include "redvault/util/text.hpp"

namespace redvault::data {

namespace fs = std::filesystem;
namespace sc = crypto::stories;
namespace ms = model::stories;
using Json = nlohmann::json;

namespace {

bool isUserId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return c == '-' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
}

std::optional<std::string> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

/// Written beside the real name and renamed over it, so a crash never leaves half a draft.
void writeFileAtomic(const fs::path& path, std::string_view bytes) {
    fs::path temp = path;
    temp += ".tmp";
    {
        util::makePrivateFile(temp);
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Couldn't save the draft.");
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) throw std::runtime_error("Couldn't save the draft.");
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        throw std::runtime_error("Couldn't save the draft.");
    }
}

fs::path recordPath(const fs::path& dir, std::string_view id) {
    return dir / (std::string(id) + ".draft");
}
fs::path mediaPath(const fs::path& dir, std::string_view id) {
    return dir / (std::string(id) + ".media");
}

struct Record {
    std::string userId;
    std::int64_t updatedAtMs = 0;
    crypto::Bytes nonce;
    crypto::Bytes sealed;
};

std::optional<Record> readRecord(const fs::path& path) {
    const auto text = readFile(path);
    if (!text) return std::nullopt;
    try {
        const Json j = Json::parse(*text);
        Record r;
        r.userId = j.at("userId").get<std::string>();
        r.updatedAtMs = j.at("updatedAt").get<std::int64_t>();
        r.nonce = crypto::base64Decode(j.at("nonce").get<std::string>());
        r.sealed = crypto::base64Decode(j.at("sealed").get<std::string>());
        return r;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// Opens the details of the draft saved under `id` (the name it is stored under, which is what it was sealed for).
std::optional<ms::SealedDraft> unseal(std::string_view userId, crypto::ByteView privateKey, std::string_view id, const Record& record) {
    if (record.userId != userId) return std::nullopt;
    try {
        return ms::parseSealedDraft(sc::openDraft(userId, id, record.sealed, record.nonce, privateKey));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void dropFiles(const fs::path& dir, std::string_view id) {
    std::error_code ec;
    fs::remove(recordPath(dir, id), ec);
    fs::remove(mediaPath(dir, id), ec);
}

std::string newDraftId() {
    return crypto::toHex(crypto::randomBytes(12));
}

std::int64_t orNow(std::int64_t nowMs) {
    return nowMs != 0 ? nowMs : util::nowMillis();
}

}  // namespace

bool isDraftId(std::string_view id) {
    return id.size() == 24 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

StoryDraftStore::StoryDraftStore(fs::path root) : root_(std::move(root)) {}

fs::path StoryDraftStore::folderOf(std::string_view userId) const {
    return isUserId(userId) ? root_ / std::string(userId) : fs::path();
}

std::vector<ms::DraftSummary> StoryDraftStore::list(std::string_view userId, crypto::ByteView privateKey, std::int64_t nowMs) const {
    std::vector<ms::DraftSummary> out;
    const fs::path dir = folderOf(userId);
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    const std::int64_t now = orNow(nowMs);
    std::vector<std::string> drop;
    std::vector<fs::path> strays;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const fs::path name = entry.path().filename();
        const std::string stem = name.stem().string();
        const std::string ext = name.extension().string();
        if (ext == ".draft" && isDraftId(stem)) {
            const auto record = readRecord(entry.path());
            const auto sealed = record && now - record->updatedAtMs <= kKeepMs ? unseal(userId, privateKey, stem, *record) : std::nullopt;
            if (!sealed) {
                drop.push_back(stem);
                continue;
            }
            out.push_back({stem, record->updatedAtMs, sealed->kind, sealed->preview, sealed->stickerCount});
        } else if (ext == ".media" && isDraftId(stem)) {
            if (!fs::exists(recordPath(dir, stem), ec)) strays.push_back(entry.path());  // a photo whose draft is gone
        } else if (ext == ".tmp") {
            strays.push_back(entry.path());  // a save that never finished
        }
    }
    for (const auto& id : drop) dropFiles(dir, id);
    for (const auto& path : strays) fs::remove(path, ec);
    std::sort(out.begin(), out.end(), [](const ms::DraftSummary& a, const ms::DraftSummary& b) { return a.updatedAtMs > b.updatedAtMs; });
    return out;
}

std::string StoryDraftStore::save(std::string_view userId, crypto::ByteView privateKey, const ms::DraftContent& content,
                                  const std::optional<DraftMediaInput>& media, const std::optional<std::string>& givenId, std::int64_t nowMs) const {
    const fs::path dir = folderOf(userId);
    if (dir.empty()) throw std::invalid_argument("Not an account id");
    if (givenId && !isDraftId(*givenId)) throw std::invalid_argument("Not a draft id");
    const std::string id = givenId ? *givenId : newDraftId();
    std::error_code ec;
    util::makePrivateDir(dir, ec);
    if (ec) throw std::runtime_error("Couldn't save the draft.");

    // The photo or video first (encrypted, its key goes inside the sealed details).
    std::optional<ms::DraftMediaRef> ref;
    if (media) {
        std::istringstream in(media->bytes);
        std::ostringstream encrypted;
        const auto file = crypto::files::encrypt(in, encrypted);
        writeFileAtomic(mediaPath(dir, id), encrypted.str());
        ms::DraftMediaRef r;
        r.kind = media->kind;
        r.key = crypto::base64Encode(file.key);
        r.header = crypto::base64Encode(file.header);
        r.sha256 = file.ciphertextSha256;
        r.encryptedSize = file.encryptedSize;
        r.chunkSize = crypto::files::kChunkBytes;
        r.mime = media->mime;
        r.width = media->width;
        r.height = media->height;
        r.durationMs = media->durationMs;
        ref = std::move(r);
    } else {
        fs::remove(mediaPath(dir, id), ec);  // saved over a draft that had one
    }
    const std::string json = ms::toJson(ms::makeSealedDraft(content, ref));
    const auto sealed = sc::sealDraft(userId, id, json, privateKey);

    // Ten at most: when this is a new one, the oldest goes (one that can't even be read goes first).
    if (!fs::exists(recordPath(dir, id), ec)) {
        std::vector<std::pair<std::int64_t, std::string>> existing;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            const fs::path name = entry.path().filename();
            if (name.extension() != ".draft" || !isDraftId(name.stem().string())) continue;
            const auto record = readRecord(entry.path());
            existing.emplace_back(record ? record->updatedAtMs : 0, name.stem().string());
        }
        std::sort(existing.begin(), existing.end());
        for (std::size_t i = 0; existing.size() - i >= kMaxDrafts; ++i) dropFiles(dir, existing[i].second);
    }

    Json record{{"v", 1},
                {"id", id},
                {"userId", std::string(userId)},
                {"updatedAt", orNow(nowMs)},
                {"nonce", crypto::base64Encode(sealed.nonce)},
                {"sealed", crypto::base64Encode(sealed.ciphertext)},
                {"hasMedia", media.has_value()}};
    writeFileAtomic(recordPath(dir, id), record.dump());
    return id;
}

std::optional<LoadedDraft> StoryDraftStore::load(std::string_view userId, crypto::ByteView privateKey, std::string_view id) const {
    const fs::path dir = folderOf(userId);
    if (dir.empty() || !isDraftId(id)) return std::nullopt;
    const auto record = readRecord(recordPath(dir, id));
    if (!record || record->userId != userId) return std::nullopt;
    const auto sealed = unseal(userId, privateKey, id, *record);
    if (!sealed) {
        dropFiles(dir, id);  // changed on disk, moved, or sealed with another key: it can't be used
        return std::nullopt;
    }
    LoadedDraft out;
    out.content = sealed->content;
    if (sealed->media) {
        const ms::DraftMediaRef& m = *sealed->media;
        const auto file = readFile(mediaPath(dir, id));
        try {
            if (!file) throw std::runtime_error("no photo");
            std::istringstream in(*file);
            std::ostringstream plain;
            crypto::files::decrypt(in, plain, crypto::base64Decode(m.key), crypto::base64Decode(m.header), m.encryptedSize, m.sha256);
            DraftMediaInput media;
            media.kind = m.kind;
            media.mime = m.mime;
            media.width = m.width;
            media.height = m.height;
            media.durationMs = m.durationMs;
            media.bytes = plain.str();
            out.media = std::move(media);
        } catch (const std::exception&) {
            dropFiles(dir, id);
            return std::nullopt;
        }
    }
    return out;
}

void StoryDraftStore::remove(std::string_view userId, std::string_view id) const {
    const fs::path dir = folderOf(userId);
    if (dir.empty() || !isDraftId(id)) return;
    dropFiles(dir, id);
}

}  // namespace redvault::data

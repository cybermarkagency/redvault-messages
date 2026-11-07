#include "redvault/data/drive_repository.hpp"

#include <sodium.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <streambuf>
#include <system_error>
#include <thread>

#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/call_crypto.hpp"
#include "redvault/crypto/drive_crypto.hpp"
#include "redvault/crypto/file_crypto.hpp"
#include "redvault/crypto/vault_crypto.hpp"
#include "redvault/data/file_repository.hpp"
#include "redvault/data/transfer.hpp"
#include "redvault/model/file_message.hpp"
#include "redvault/util/private_files.hpp"
#include "redvault/util/text.hpp"

namespace redvault::data {

namespace fs = std::filesystem;
using api::ApiError;
using api::TransferInterrupted;
using api::UserFacingError;
using Json = nlohmann::json;

namespace {

constexpr int kMaxAttempts = 4;

fs::path pathFromUtf8(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8FromPath(const fs::path& path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

void removeQuietly(const fs::path& path) {
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

std::string planName(const std::string& tier) {
    if (tier == "premium") return "Premium";
    if (tier == "pro") return "Pro";
    return "Free";
}

/// "1.5 GB": the server's formatBytes, so both word storage the same way.
std::string formatBytes(std::int64_t bytes) {
    static const char* units[] = {"bytes", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    char buffer[32];
    if (unit == 0 || value >= 10) std::snprintf(buffer, sizeof buffer, "%.0f %s", value, units[unit]);
    else std::snprintf(buffer, sizeof buffer, "%.1f %s", value, units[unit]);
    return buffer;
}

/// Reads through another streambuf, counting bytes (for upload progress). Ends early once `cancelled` is set.
class CountingInBuf : public std::streambuf {
public:
    CountingInBuf(std::streambuf* source, std::function<void(std::uint64_t)> onCount, const std::atomic<bool>* cancelled = nullptr)
        : source_(source), onCount_(std::move(onCount)), cancelled_(cancelled) {
        setg(buffer_, buffer_, buffer_);
    }

protected:
    int_type underflow() override {
        // A short body makes the transfer fail; the caller then sees the cancellation.
        if (cancelled_ != nullptr && cancelled_->load()) return traits_type::eof();
        const std::streamsize n = source_->sgetn(buffer_, sizeof buffer_);
        if (n <= 0) return traits_type::eof();
        count_ += static_cast<std::uint64_t>(n);
        if (onCount_) onCount_(count_);
        setg(buffer_, buffer_, buffer_ + n);
        return traits_type::to_int_type(buffer_[0]);
    }

private:
    std::streambuf* source_;
    std::function<void(std::uint64_t)> onCount_;
    const std::atomic<bool>* cancelled_;
    char buffer_[64 * 1024];
    std::uint64_t count_ = 0;
};

/// Writes through to another streambuf, counting bytes (for download progress). Refuses more once `cancelled` is set.
class CountingOutBuf : public std::streambuf {
public:
    CountingOutBuf(std::streambuf* sink, std::function<void(std::uint64_t)> onCount, const std::atomic<bool>* cancelled = nullptr,
                   std::uint64_t start = 0)
        : sink_(sink), onCount_(std::move(onCount)), cancelled_(cancelled), count_(start) {}

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (cancelled_ != nullptr && cancelled_->load()) return 0;
        const auto written = sink_->sputn(s, n);
        count_ += static_cast<std::uint64_t>(std::max<std::streamsize>(written, 0));
        if (onCount_) onCount_(count_);
        return written;
    }
    int_type overflow(int_type c) override {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        const char ch = traits_type::to_char_type(c);
        return xsputn(&ch, 1) == 1 ? c : traits_type::eof();
    }

private:
    std::streambuf* sink_;
    std::function<void(std::uint64_t)> onCount_;
    const std::atomic<bool>* cancelled_;
    std::uint64_t count_ = 0;
};

constexpr char kJournalKdfContext[crypto_kdf_CONTEXTBYTES + 1] = "rvxfer01";

std::string sha256Hex(const void* data, std::size_t size) {
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, static_cast<const unsigned char*>(data), size);
    char hex[crypto_hash_sha256_BYTES * 2 + 1];
    sodium_bin2hex(hex, sizeof hex, digest, sizeof digest);
    return hex;
}

/// Part `n` (1-based) of a file of `total` bytes cut into `partSize` pieces.
std::uint64_t partLength(std::uint64_t total, std::uint64_t partSize, int partCount, int n) {
    return n < partCount ? partSize : total - partSize * static_cast<std::uint64_t>(partCount - 1);
}

void throwIfCancelled(const std::shared_ptr<TransferControl>& control) {
    if (control && control->cancelled.load()) throw TransferCancelled("Cancelled.");
}

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
        if (code == "backup_storage_full" || code == "backup_too_large" || code == "too_many_pending_uploads" ||
            code == "rate_limited" || code == "backup_deleted" || code == "upload_expired") {
            throw UserFacingError(e.what());
        }
        if (code == "backup_disabled") throw UserFacingError("Files aren't available on this server.");
        if (code == "network_error") throw TransferInterrupted("Network interrupted. The transfer resumes when the connection is back.");
        if (code == "tor_unavailable") throw TransferInterrupted("Tor isn't connected. The transfer resumes when it is.");
        if (e.status() >= 500 || code == "upload_incomplete") throw TransferInterrupted("The server is busy. The transfer resumes shortly.");
        if (e.status() == 404) throw UserFacingError("That file isn't there anymore.");
        throw UserFacingError(fallback);
    } catch (const net::NetworkError&) {
        throw TransferInterrupted("Network interrupted. The transfer resumes when the connection is back.");
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

}  // namespace

DriveRepository::DriveRepository(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const SelfIdentity> identity, fs::path cacheDir,
                                 Sleep sleep, std::shared_ptr<ContactKeyPins> contacts)
    : api_(std::move(api)),
      identity_(std::move(identity)),
      contacts_(std::move(contacts)),
      cacheDir_(std::move(cacheDir)),
      sleep_(sleep ? std::move(sleep) : Sleep([](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); })) {}

DriveEntry DriveRepository::decode(const api::drive::Entry& e) const {
    const auto key = crypto::drive::driveKey(identity_->privateKey.view());
    const std::string json =
        crypto::drive::openMeta(crypto::base64Decode(e.metaCiphertext), crypto::base64Decode(e.metaNonce), key.view());
    DriveEntry out;
    out.id = e.id;
    out.kind = e.kind;
    out.encryptedSize = e.encryptedSize;
    out.sharedWith = e.sharedWith;
    out.meta = Json::parse(json);
    out.name = out.meta.value("name", std::string("Untitled"));
    out.parent = out.meta.value("parent", std::string());
    out.mimeType = out.meta.value("mime", std::string("application/octet-stream"));
    out.size = out.meta.value("size", std::int64_t{0});
    out.modifiedMs = out.meta.value("modified", std::int64_t{0});
    out.fileId = out.meta.value("fileId", e.id);
    out.version = out.meta.value("version", 1);
    out.trashed = out.meta.value("trashed", false);
    out.trashedMs = out.meta.value("trashedAt", std::int64_t{0});
    return out;
}

std::string DriveRepository::sealedMetaJson(const DriveEntry& entry) const {
    Json meta = entry.meta.is_object() ? entry.meta : Json::object();
    meta["v"] = 1;
    meta["kind"] = entry.kind;
    meta["name"] = entry.name;
    meta["parent"] = entry.parent;
    meta["modified"] = entry.modifiedMs;
    meta["trashed"] = entry.trashed;
    meta["trashedAt"] = entry.trashedMs;
    if (entry.kind == "file") {
        meta["mime"] = entry.mimeType;
        meta["size"] = entry.size;
        meta["fileId"] = entry.fileId;
        meta["version"] = entry.version;
    }
    // A locked file's key is only ever stored locked: an unlocked copy's key never goes back to the server.
    if (filelock::isLocked(meta)) meta.erase("key");
    return meta.dump();
}

std::string DriveRepository::recipientMetaJson(const DriveEntry& entry) const {
    Json meta = Json::parse(sealedMetaJson(entry));
    meta.erase("lock");
    if (entry.meta.contains("key")) meta["key"] = entry.meta.at("key");
    return meta.dump();
}

DriveSnapshot DriveRepository::refresh() {
    return mapErrors("Couldn't load your files. Try again.", [&] {
        DriveSnapshot out;
        const auto listing = api::drive::list(*api_);
        out.usage = listing.usage;
        for (const auto& e : listing.items) {
            try {
                out.entries.push_back(decode(e));
            } catch (const std::exception&) {
                // Not openable with this key (should never happen): leave it out rather than show garbage.
            }
        }
        for (const auto& s : api::drive::sharedWithMe(*api_)) {
            try {
                const std::string json = crypto::vault::openFromSender(crypto::base64Decode(s.sealedMeta), identity_->publicKey,
                                                                       identity_->privateKey.view());
                DriveEntry entry;
                entry.id = s.id;
                entry.kind = "file";
                entry.incoming = true;
                entry.ownerId = s.ownerId;
                entry.encryptedSize = s.encryptedSize;
                entry.meta = Json::parse(json);
                entry.name = entry.meta.value("name", std::string("Untitled"));
                entry.mimeType = entry.meta.value("mime", std::string("application/octet-stream"));
                entry.size = entry.meta.value("size", std::int64_t{0});
                entry.modifiedMs = entry.meta.value("modified", std::int64_t{0});
                entry.fileId = entry.meta.value("fileId", s.id);
                entry.version = entry.meta.value("version", 1);
                out.incoming.push_back(std::move(entry));
            } catch (const std::exception&) {
            }
        }
        out.links = api::drive::links(*api_);
        return out;
    });
}

DriveEntry DriveRepository::createFolder(const std::string& name, const std::string& parent) {
    return mapErrors("Couldn't create the folder. Try again.", [&] {
        DriveEntry folder;
        folder.kind = "folder";
        folder.name = model::file_message::sanitizeFilename(name.empty() ? "New folder" : name);
        folder.parent = parent;
        folder.modifiedMs = util::nowMillis();
        const auto key = crypto::drive::driveKey(identity_->privateKey.view());
        const auto sealed = crypto::drive::sealMeta(sealedMetaJson(folder), key.view());
        return decode(api::drive::createFolder(*api_, crypto::base64Encode(sealed.ciphertext), crypto::base64Encode(sealed.nonce)));
    });
}

namespace {

void requireUnlocked(const DriveEntry& entry) {
    if (entry.needsUnlock()) throw UserFacingError("This file is locked. Unlock it with its key or password first.");
}

}  // namespace

// --- Resumable transfers (docs/tor-routing-plan.md) ---

crypto::SecretBytes DriveRepository::deriveJournalKey(const crypto::SecretBytes& accountPrivateKey) {
    crypto::ensureSodium();
    if (accountPrivateKey.size() < crypto_kdf_KEYBYTES) throw std::invalid_argument("account private key too short");
    crypto::SecretBytes key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
    crypto_kdf_derive_from_key(key.data(), key.size(), 1, kJournalKdfContext, accountPrivateKey.data());
    return key;
}

void DriveRepository::enableResumable(fs::path journalDir, crypto::SecretBytes journalKey) {
    if (journalKey.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) throw std::invalid_argument("journal key must be 32 bytes");
    util::makePrivateDir(journalDir / "downloads");
    journalDir_ = std::move(journalDir);
    journalKey_ = std::move(journalKey);
}

void DriveRepository::writeJournal(const std::string& objectId, const Json& journal) const {
    // Sealed: it holds the file's key. The object id is bound in, so records can't be swapped.
    const std::string plain = journal.dump();
    const std::string ad = "drive-upload:" + objectId;
    std::string sealed(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    auto* nonce = reinterpret_cast<unsigned char*>(sealed.data());
    randombytes_buf(nonce, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    unsigned long long length = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(sealed.data()) + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,
                                               &length, reinterpret_cast<const unsigned char*>(plain.data()), plain.size(),
                                               reinterpret_cast<const unsigned char*>(ad.data()), ad.size(), nullptr, nonce,
                                               journalKey_.data());
    const fs::path target = journalDir_ / (objectId + ".upload");
    const fs::path temp = target.string() + ".tmp";
    {
        util::makePrivateFile(temp);
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(sealed.data(), static_cast<std::streamsize>(sealed.size()));
        if (!out) throw UserFacingError("Couldn't save the upload's progress.");
    }
    fs::rename(temp, target);
}

std::optional<Json> DriveRepository::readJournal(const std::string& objectId) const {
    std::ifstream in(journalDir_ / (objectId + ".upload"), std::ios::binary);
    if (!in) return std::nullopt;
    const std::string sealed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (sealed.size() < crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + crypto_aead_xchacha20poly1305_ietf_ABYTES) return std::nullopt;
    const std::string ad = "drive-upload:" + objectId;
    std::string plain(sealed.size() - crypto_aead_xchacha20poly1305_ietf_NPUBBYTES - crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(plain.data()), &length, nullptr,
            reinterpret_cast<const unsigned char*>(sealed.data()) + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,
            sealed.size() - crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, reinterpret_cast<const unsigned char*>(ad.data()), ad.size(),
            reinterpret_cast<const unsigned char*>(sealed.data()), journalKey_.data()) != 0) {
        return std::nullopt;
    }
    plain.resize(length);
    try {
        return Json::parse(plain);
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

std::vector<PendingUpload> DriveRepository::pendingUploads() const {
    std::vector<std::pair<std::int64_t, PendingUpload>> found;
    if (!resumable()) return {};
    std::error_code error;
    for (const auto& item : fs::directory_iterator(journalDir_, error)) {
        if (item.path().extension() != ".upload") continue;
        const std::string id = item.path().stem().string();
        if (const auto j = readJournal(id)) {
            found.emplace_back(j->value("createdMs", std::int64_t{0}),
                               PendingUpload{id, j->value("name", ""), j->value("parent", ""), j->value("size", std::int64_t{0}),
                                             j->value("partCount", 0)});
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<PendingUpload> out;
    for (auto& [_, p] : found) out.push_back(std::move(p));
    return out;
}

DriveEntry DriveRepository::sendParts(const Json& journal, const ProgressFn& onProgress, const std::shared_ptr<TransferControl>& control) {
    const std::string objectId = journal.at("objectId").get<std::string>();
    const auto total = journal.at("encryptedSize").get<std::uint64_t>();
    const auto partSize = journal.at("partSize").get<std::uint64_t>();
    const int partCount = journal.at("partCount").get<int>();
    const auto& hashes = journal.at("partSha256");
    const fs::path ciphertext = journalDir_ / (objectId + ".ciphertext");

    // What the server already has (and has right): only the rest goes again.
    std::set<int> have;
    std::uint64_t done = 0;
    for (const auto& part : api::drive::listParts(*api_, objectId)) {
        if (part.partNumber >= 1 && part.partNumber <= partCount && part.sha256 == hashes.at(part.partNumber - 1).get<std::string>()) {
            have.insert(part.partNumber);
            done += part.size;
        }
    }
    if (onProgress) onProgress(DriveStage::Uploading, done, total);
    std::ifstream file(ciphertext, std::ios::binary);
    if (!file) throw UserFacingError("The upload's encrypted copy is missing. Start it again.");
    std::string chunk;
    for (int n = 1; n <= partCount; ++n) {
        if (have.count(n)) continue;
        throwIfCancelled(control);
        const std::uint64_t length = partLength(total, partSize, partCount, n);
        chunk.resize(length);
        file.seekg(static_cast<std::streamoff>(partSize * static_cast<std::uint64_t>(n - 1)));
        file.read(chunk.data(), static_cast<std::streamsize>(length));
        if (static_cast<std::uint64_t>(file.gcount()) != length) throw UserFacingError("The upload's encrypted copy is damaged. Start it again.");
        const std::string hash = hashes.at(n - 1).get<std::string>();
        // Checked again here: a copy changed on disk must not go up as this part.
        if (sha256Hex(chunk.data(), chunk.size()) != hash) throw UserFacingError("The upload's encrypted copy is damaged. Start it again.");
        api::TransferTargetDto target{"proxy", "/backup/" + objectId + "/parts/" + std::to_string(n), "PUT",
                                      {{"Content-Type", "application/octet-stream"}, {"X-Content-Sha256", hash}}, ""};
        const std::uint64_t before = done;
        transferTarget(
            *api_, target, "PUT", "/backup/",
            [&](const net::HttpRequest& request) {
                // Not retried as a network failure: a cancelled transfer stops here.
                throwIfCancelled(control);
                std::stringbuf source(chunk, std::ios::in);
                CountingInBuf counting(
                    &source, [&](std::uint64_t sent) {
                        if (onProgress) onProgress(DriveStage::Uploading, before + sent, total);
                    },
                    control ? &control->cancelled : nullptr);
                std::istream body(&counting);
                try {
                    return api_->transport().sendStreaming(request, {.upload = &body, .uploadSize = length});
                } catch (const net::NetworkError&) {
                    throwIfCancelled(control);  // the short body was the cancellation, not the network
                    throw;
                }
            },
            sleep_);
        throwIfCancelled(control);
        done += length;
    }
    for (int attempt = 0;; ++attempt) {
        try {
            api::drive::completeUpload(*api_, objectId);
            break;
        } catch (const ApiError& e) {
            const bool transient = e.code() == "network_error" || e.status() >= 500 || e.code() == "upload_incomplete";
            if (!transient || attempt + 1 >= kMaxAttempts) throw;
            sleep_(std::chrono::milliseconds(400LL << attempt));
        }
    }
    DriveEntry entry;
    entry.id = objectId;
    entry.kind = "file";
    entry.name = journal.value("name", "");
    entry.parent = journal.value("parent", "");
    entry.mimeType = journal.value("mimeType", "");
    entry.size = journal.value("size", std::int64_t{0});
    entry.encryptedSize = static_cast<std::int64_t>(total);
    entry.modifiedMs = journal.value("modifiedMs", std::int64_t{0});
    entry.fileId = journal.value("fileId", "");
    entry.version = journal.value("version", 1);
    entry.meta = journal.value("meta", Json::object());
    // Done: the journal and the encrypted copy go.
    file.close();
    removeQuietly(ciphertext);
    removeQuietly(journalDir_ / (objectId + ".upload"));
    return entry;
}

DriveEntry DriveRepository::resumeUpload(const std::string& objectId, const ProgressFn& onProgress, std::shared_ptr<TransferControl> control) {
    return mapErrors("Upload failed. Try again.", [&] {
        const auto journal = readJournal(objectId);
        if (!journal) throw UserFacingError("This upload can't be resumed. Start it again.");
        try {
            return sendParts(*journal, onProgress, control);
        } catch (const TransferCancelled&) {
            abandonUpload(objectId);
            throw;
        } catch (const ApiError& e) {
            // Gone for good on the server (expired, deleted, refused): nothing left to resume.
            if (e.status() == 404 || e.status() == 410 || e.status() == 400) {
                removeQuietly(journalDir_ / (objectId + ".ciphertext"));
                removeQuietly(journalDir_ / (objectId + ".upload"));
            }
            throw;
        }
    });
}

void DriveRepository::abandonUpload(const std::string& objectId) {
    try {
        api::drive::remove(*api_, objectId);
    } catch (const std::exception&) {
        // Unfinished uploads expire on the server anyway (runCleanup); the local copy goes regardless.
    }
    removeQuietly(journalDir_ / (objectId + ".ciphertext"));
    removeQuietly(journalDir_ / (objectId + ".upload"));
}

DriveEntry DriveRepository::upload(const fs::path& file, const std::string& parent, const std::optional<DriveEntry>& replacing,
                                   const ProgressFn& onProgress, const Json& extraMeta, const FileLock* lock,
                                   std::shared_ptr<TransferControl> control) {
    return mapErrors("Upload failed. Try again.", [&] {
        // Before any work: a weak password is refused up front.
        if (lock && lock->kind == FileLock::Kind::Password) {
            if (const auto problem = filelock::passwordProblem(lock->password)) throw UserFacingError(*problem);
        }
        std::error_code sizeError;
        const auto plainSize = fs::file_size(file, sizeError);
        if (sizeError) throw UserFacingError("Couldn't read that file.");
        const auto encryptedSize = static_cast<std::int64_t>(crypto::files::encryptedSizeFor(plainSize));

        // Say so right away, before spending minutes encrypting something that can't fit.
        const auto usage = api::drive::usage(*api_);
        if (usage.used + encryptedSize > usage.limit) {
            const std::string tier = usage.tier;
            const std::string upgrade = tier == "premium" ? "" : tier == "pro" ? ", or upgrade to Premium for more space"
                                                                                  : ", or upgrade to Pro or Premium for more space";
            throw UserFacingError("Not enough storage: this file needs " + formatBytes(encryptedSize) + " and you have " +
                                  formatBytes(usage.remaining) + " left of your " + formatBytes(usage.limit) + " (" + planName(tier) +
                                  "). Delete some files to free up space" + upgrade + ".");
        }

        const fs::path outgoing = cacheDir_ / "outgoing";
        util::makePrivateDir(outgoing);
        const fs::path ciphertext = outgoing / util::randomUuid();
        struct Cleanup {
            fs::path path;
            ~Cleanup() { removeQuietly(path); }
        } cleanup{ciphertext};

        crypto::files::EncryptedFile encrypted;
        {
            std::ifstream input(file, std::ios::binary);
            if (!input) throw UserFacingError("Couldn't read that file.");
            util::makePrivateFile(ciphertext);
            std::ofstream output(ciphertext, std::ios::binary | std::ios::trunc);
            if (!output) throw UserFacingError("Upload failed. Try again.");
            encrypted = crypto::files::encrypt(input, output, [&](std::uint64_t done) {
                if (onProgress) onProgress(DriveStage::Encrypting, done, plainSize);
            });
            output.flush();
            if (!output || input.bad()) throw UserFacingError("Couldn't read that file.");
        }

        DriveEntry entry;
        entry.kind = "file";
        entry.name = model::file_message::sanitizeFilename(utf8FromPath(file.filename()));
        entry.parent = parent;
        entry.mimeType = FileRepository::guessMimeType(file);
        entry.size = static_cast<std::int64_t>(encrypted.plaintextSize);
        entry.modifiedMs = util::nowMillis();
        entry.fileId = replacing ? replacing->fileId : util::randomUuid();
        entry.version = replacing ? replacing->version + 1 : 1;
        if (replacing) {
            entry.name = replacing->name;
            entry.parent = replacing->parent;
        }
        entry.meta = extraMeta.is_object() ? extraMeta : Json::object();
        entry.meta.erase("lock");
        entry.meta["key"] = crypto::base64Encode(encrypted.key);
        if (lock) entry.meta["lock"] = filelock::lock(encrypted.key, *lock, entry.fileId);
        entry.meta["header"] = crypto::base64Encode(encrypted.header);
        entry.meta["chunk"] = static_cast<int>(crypto::files::kChunkBytes);
        entry.meta["sha"] = encrypted.ciphertextSha256;
        const auto key = crypto::drive::driveKey(identity_->privateKey.view());
        const auto sealed = crypto::drive::sealMeta(sealedMetaJson(entry), key.view());

        throwIfCancelled(control);
        if (resumable()) {
            // In parts: each part's SHA-256, and the manifest over them all.
            const std::uint64_t total = encrypted.encryptedSize;
            const int partCount = static_cast<int>((total + kPartSize - 1) / kPartSize);
            Json hashes = Json::array();
            std::string manifest;
            {
                std::ifstream in(ciphertext, std::ios::binary);
                std::string chunk;
                for (int n = 1; n <= partCount; ++n) {
                    const std::uint64_t length = partLength(total, kPartSize, partCount, n);
                    chunk.resize(length);
                    in.read(chunk.data(), static_cast<std::streamsize>(length));
                    unsigned char digest[crypto_hash_sha256_BYTES];
                    crypto_hash_sha256(digest, reinterpret_cast<const unsigned char*>(chunk.data()), chunk.size());
                    manifest.append(reinterpret_cast<const char*>(digest), sizeof digest);
                    char hex[crypto_hash_sha256_BYTES * 2 + 1];
                    sodium_bin2hex(hex, sizeof hex, digest, sizeof digest);
                    hashes.push_back(std::string(hex));
                }
            }
            const auto auth = api::drive::requestUploadParts(*api_, static_cast<std::int64_t>(total), encrypted.ciphertextSha256,
                                                             crypto::base64Encode(sealed.ciphertext), crypto::base64Encode(sealed.nonce),
                                                             kPartSize, sha256Hex(manifest.data(), manifest.size()));
            if (auth.upload.mode != "parts" || auth.partCount != partCount) throw UserFacingError("Upload failed. Try again.");
            Json journal{{"objectId", auth.objectId},
                         {"name", entry.name},
                         {"parent", entry.parent},
                         {"mimeType", entry.mimeType},
                         {"size", entry.size},
                         {"modifiedMs", entry.modifiedMs},
                         {"fileId", entry.fileId},
                         {"version", entry.version},
                         {"meta", entry.meta},
                         {"encryptedSize", total},
                         {"partSize", kPartSize},
                         {"partCount", partCount},
                         {"partSha256", hashes},
                         {"createdMs", util::nowMillis()}};
            // The encrypted copy and the sealed journal outlive this session: a restart resumes them.
            fs::copy_file(ciphertext, journalDir_ / (auth.objectId + ".ciphertext"), fs::copy_options::overwrite_existing);
            writeJournal(auth.objectId, journal);
            try {
                return sendParts(journal, onProgress, control);
            } catch (const TransferCancelled&) {
                abandonUpload(auth.objectId);
                throw;
            }
        }

        const auto auth = api::drive::requestUpload(*api_, static_cast<std::int64_t>(encrypted.encryptedSize), encrypted.ciphertextSha256,
                                                    crypto::base64Encode(sealed.ciphertext), crypto::base64Encode(sealed.nonce));
        if (auth.upload.method != "PUT") throw UserFacingError("Upload failed. Try again.");
        const std::uint64_t total = encrypted.encryptedSize;
        transferTarget(
            *api_, auth.upload, "PUT", "/backup/",
            [&](const net::HttpRequest& request) {
                std::filebuf source;
                if (!source.open(ciphertext, std::ios::in | std::ios::binary)) throw UserFacingError("Upload failed. Try again.");
                CountingInBuf counting(&source, [&](std::uint64_t done) {
                    if (onProgress) onProgress(DriveStage::Uploading, done, total);
                });
                std::istream body(&counting);
                return api_->transport().sendStreaming(request, {.upload = &body, .uploadSize = total});
            },
            sleep_);
        for (int attempt = 0;; ++attempt) {
            try {
                api::drive::completeUpload(*api_, auth.objectId);
                break;
            } catch (const ApiError& e) {
                const bool transient = e.code() == "network_error" || e.status() >= 500 || e.code() == "upload_incomplete";
                if (!transient || attempt + 1 >= kMaxAttempts) throw;
                sleep_(std::chrono::milliseconds(400LL << attempt));
            }
        }
        entry.id = auth.objectId;
        entry.encryptedSize = static_cast<std::int64_t>(encrypted.encryptedSize);
        return entry;
    });
}

DriveEntry DriveRepository::unlock(const DriveEntry& entry, const FileLock& with) const {
    if (!entry.locked()) return entry;
    const auto key = filelock::unlock(entry.meta.at("lock"), with, entry.fileId);
    DriveEntry out = entry;
    out.meta["key"] = crypto::base64Encode(key.view());
    return out;
}

DriveEntry DriveRepository::setLock(const DriveEntry& entry, const FileLock* lock) {
    requireUnlocked(entry);
    if (entry.kind != "file" || entry.incoming) throw UserFacingError("Only your own files can be locked.");
    DriveEntry changed = entry;
    if (lock) {
        if (lock->kind == FileLock::Kind::Password) {
            if (const auto problem = filelock::passwordProblem(lock->password)) throw UserFacingError(*problem);
        }
        const auto fileKey = crypto::base64Decode(entry.meta.at("key").get<std::string>());
        changed.meta["lock"] = mapErrors("Couldn't lock the file. Try again.", [&] { return filelock::lock(fileKey, *lock, entry.fileId); });
    } else {
        changed.meta.erase("lock");
    }
    DriveEntry saved = save(changed);
    // Still usable in this session.
    saved.meta["key"] = entry.meta.at("key");
    return saved;
}

void DriveRepository::download(const DriveEntry& entry, const fs::path& destination, const ProgressFn& onProgress,
                               std::shared_ptr<TransferControl> control) {
    requireUnlocked(entry);
    mapErrors("Download failed. Try again.", [&] {
        const auto target = api::drive::download(*api_, entry.id);
        if (target.method != "GET") throw UserFacingError("Download failed. Try again.");
        const fs::path folder = cacheDir_ / "incoming" / util::randomUuid();
        util::makePrivateDir(folder);
        struct Cleanup {
            fs::path path;
            ~Cleanup() { removeQuietly(path); }
        } cleanup{folder};
        // Resumable: the ciphertext (still end-to-end encrypted) is kept between attempts and
        // runs, and each attempt asks only for what's missing.
        const fs::path ciphertext = resumable() ? journalDir_ / "downloads" / (entry.id + ".ciphertext") : folder / ".ciphertext";
        const auto total = static_cast<std::uint64_t>(entry.encryptedSize);
        try {
            transferTarget(
                *api_, target, "GET", "/backup/",
                [&](const net::HttpRequest& request) {
                    throwIfCancelled(control);
                    std::error_code sizeError;
                    std::uint64_t have = resumable() && fs::exists(ciphertext) ? fs::file_size(ciphertext, sizeError) : 0;
                    if (sizeError || have > total) have = 0;
                    net::HttpRequest ranged = request;
                    if (have > 0 && have < total) ranged.headers.emplace_back("range", "bytes=" + std::to_string(have) + "-");
                    if (have == total && total > 0) return net::HttpResponse{206, {}, {}};
                    std::filebuf sinkFile;
                    const auto mode = std::ios::out | std::ios::binary | (have > 0 ? std::ios::app : std::ios::trunc);
                    util::makePrivateFile(ciphertext);
                    if (!sinkFile.open(ciphertext, mode)) throw UserFacingError("Download failed. Try again.");
                    CountingOutBuf counting(
                        &sinkFile, [&](std::uint64_t done) {
                            if (onProgress) onProgress(DriveStage::Downloading, done, total);
                        },
                        control ? &control->cancelled : nullptr, have);
                    std::ostream sink(&counting);
                    auto response = api_->transport().sendStreaming(ranged, {.download = &sink});
                    sink.flush();
                    sinkFile.close();
                    if (response.status == 200 && have > 0) {
                        // The server sent everything instead of the rest: keep just the full body.
                        const fs::path whole = ciphertext.string() + ".whole";
                        {
                            std::ifstream in(ciphertext, std::ios::binary);
                            in.seekg(static_cast<std::streamoff>(have));
                            util::makePrivateFile(whole);
                            std::ofstream out(whole, std::ios::binary | std::ios::trunc);
                            out << in.rdbuf();
                        }
                        fs::rename(whole, ciphertext);
                    }
                    return response;
                },
                sleep_);
        } catch (...) {
            if (control && control->cancelled.load()) {
                removeQuietly(ciphertext);
                throw TransferCancelled("Cancelled.");
            }
            throw;
        }
        throwIfCancelled(control);
        std::error_code sizeError;
        if (fs::file_size(ciphertext, sizeError) != total || sizeError) {
            removeQuietly(ciphertext);
            throw UserFacingError("Download failed. Try again.");
        }
        struct DropAfter {
            fs::path path;
            ~DropAfter() { removeQuietly(path); }
        } drop{resumable() ? ciphertext : fs::path()};

        if (onProgress) onProgress(DriveStage::Decrypting, 0, total);
        const fs::path partial = destination.parent_path() / (destination.filename().u8string() + u8".part");
        try {
            std::ifstream input(ciphertext, std::ios::binary);
            util::makePrivateFile(partial);
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            if (!input || !out) throw UserFacingError("Download failed. Try again.");
            crypto::files::decrypt(input, out, crypto::base64Decode(entry.meta.at("key").get<std::string>()),
                                   crypto::base64Decode(entry.meta.at("header").get<std::string>()), total,
                                   entry.meta.at("sha").get<std::string>());
            out.flush();
            if (!out) throw UserFacingError("Download failed. Try again.");
            out.close();
            fs::rename(partial, destination);
        } catch (...) {
            removeQuietly(partial);
            throw;
        }
        if (onProgress) onProgress(DriveStage::Decrypting, total, total);
    });
}

fs::path DriveRepository::downloadToCache(const DriveEntry& entry, const ProgressFn& onProgress) {
    const fs::path folder = cacheDir_ / "preview" / util::randomUuid();
    util::makePrivateDir(folder);
    const fs::path destination = folder / pathFromUtf8(model::file_message::sanitizeFilename(entry.name));
    download(entry, destination, onProgress);
    return destination;
}

DriveEntry DriveRepository::save(const DriveEntry& entry) {
    return mapErrors("Couldn't save the change. Try again.", [&] {
        DriveEntry changed = entry;
        changed.name = model::file_message::sanitizeFilename(entry.name.empty() ? "Untitled" : entry.name);
        const auto key = crypto::drive::driveKey(identity_->privateKey.view());
        const auto sealed = crypto::drive::sealMeta(sealedMetaJson(changed), key.view());
        auto updated = decode(api::drive::updateMeta(*api_, entry.id, crypto::base64Encode(sealed.ciphertext), crypto::base64Encode(sealed.nonce)));
        updated.sharedWith = entry.sharedWith;
        return updated;
    });
}

void DriveRepository::remove(const DriveEntry& entry) {
    mapErrors("Couldn't delete it. Try again.", [&] { api::drive::remove(*api_, entry.id); });
}

void DriveRepository::share(const DriveEntry& entry, const std::string& recipientId) {
    requireUnlocked(entry);
    mapErrors("Couldn't share it. Try again.", [&] {
        const auto recipient = api_->getUser(recipientId);
        requireUnchangedKey(contacts_.get(), identity_->userId, recipientId, recipient.publicKey);
        const auto sealed = crypto::vault::sealForRecipient(recipientMetaJson(entry), crypto::base64Decode(recipient.publicKey));
        api::drive::share(*api_, entry.id, recipientId, crypto::base64Encode(sealed));
    });
}

void DriveRepository::unshare(const DriveEntry& entry, const std::string& recipientId) {
    mapErrors("Couldn't stop sharing it. Try again.", [&] { api::drive::unshare(*api_, entry.id, recipientId); });
}

std::string DriveRepository::createLink(DriveEntry& entry, const LinkOptions& options, const std::string& webAppBase) {
    requireUnlocked(entry);
    return mapErrors("Couldn't create the link. Try again.", [&] {
        const auto secret = crypto::calls::newLinkSecret();
        const auto salt = options.password.empty() ? crypto::Bytes{} : crypto::drive::newPasswordSalt();
        const auto key = crypto::drive::linkKey(secret.view(), options.password, salt);
        // What the viewer needs to decrypt and name the file: nothing about folders or other files.
        const Json linkMeta{{"v", 1},
                            {"name", entry.name},
                            {"mime", entry.mimeType},
                            {"size", entry.size},
                            {"key", entry.meta.at("key")},
                            {"header", entry.meta.at("header")},
                            {"chunk", entry.meta.value("chunk", 65536)},
                            {"sha", entry.meta.at("sha")}};
        const auto sealed = crypto::drive::sealMeta(linkMeta.dump(), key.view());
        const auto link = api::drive::createLink(*api_, entry.id, crypto::base64Encode(sealed.ciphertext), crypto::base64Encode(sealed.nonce),
                                                 salt.empty() ? std::nullopt : std::optional<std::string>(crypto::base64Encode(salt)),
                                                 options.allowDownload, options.expiresInDays);
        // Kept (sealed) in the file's own metadata, so "Copy link" works again later.
        entry.meta["links"][link.id] = crypto::calls::linkSecretToFragment(secret.view());
        entry = save(entry);
        return crypto::drive::fileLinkUrl(webAppBase, link.id, secret.view());
    });
}

void DriveRepository::deleteLink(DriveEntry& entry, const std::string& linkId) {
    mapErrors("Couldn't remove the link. Try again.", [&] {
        api::drive::deleteLink(*api_, linkId);
        if (entry.meta.contains("links")) {
            entry.meta["links"].erase(linkId);
            entry = save(entry);
        }
    });
}

std::string DriveRepository::linkUrl(const DriveEntry& entry, const std::string& linkId, const std::string& webAppBase) const {
    const auto links = entry.meta.find("links");
    if (links == entry.meta.end() || !links->contains(linkId)) return {};
    const auto secret = crypto::calls::linkSecretFromFragment((*links)[linkId].get<std::string>());
    if (!secret) return {};
    return crypto::drive::fileLinkUrl(webAppBase, linkId, secret->view());
}

void DriveRepository::clearCache() {
    removeQuietly(cacheDir_ / "preview");
    removeQuietly(cacheDir_ / "incoming");
    removeQuietly(cacheDir_ / "outgoing");
}

}  // namespace redvault::data

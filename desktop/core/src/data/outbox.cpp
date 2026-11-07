#include "redvault/data/outbox.hpp"

#include "redvault/util/private_files.hpp"

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace redvault::data {

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr char kItemMagic[] = "RVOB1";  // + 24-byte nonce + XChaCha20-Poly1305 ciphertext
constexpr char kFileMagic[] = "RVOF1";  // + secretstream header + sealed 64 KiB chunks
constexpr std::size_t kMagicSize = 5;
constexpr std::size_t kChunk = 64 * 1024;
constexpr char kKdfContext[crypto_kdf_CONTEXTBYTES + 1] = "rvoutbx1";
constexpr std::uint64_t kKdfSubkeyId = 1;

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/// Nanoseconds since the epoch, bumped so two calls never return the same value (or go backwards) in this run.
std::int64_t nextOrder() {
    using namespace std::chrono;
    static std::mutex mutex;
    static std::int64_t last = 0;
    std::lock_guard lock(mutex);
    last = std::max(last + 1, static_cast<std::int64_t>(duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count()));
    return last;
}

/// Only ids this class made reach the file system (no path tricks from a stray file name).
bool safeId(const std::string& id) {
    return id.size() >= 16 && id.size() <= 64 &&
           std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; });
}

std::string kindName(OutboxItem::Kind kind) {
    return kind == OutboxItem::Kind::File ? "file" : "text";
}

Json toJson(const OutboxItem& item) {
    Json j{{"id", item.id},
           {"conversationId", item.conversationId},
           {"kind", kindName(item.kind)},
           {"text", item.text},
           {"replyPreview", item.replyPreview},
           {"replyToSender", item.replyToSender},
           {"held", item.held},
           {"replyFromMe", item.replyFromMe},
           {"mimeType", item.mimeType},
           {"createdAtMs", item.createdAtMs},
           {"order", item.order},
           {"attempts", item.attempts},
           {"lastError", item.lastError}};
    if (item.replyToId) j["replyToId"] = *item.replyToId;
    return j;
}

OutboxItem fromJson(const Json& j) {
    OutboxItem item;
    item.id = j.at("id").get<std::string>();
    item.conversationId = j.at("conversationId").get<std::string>();
    item.kind = j.at("kind").get<std::string>() == "file" ? OutboxItem::Kind::File : OutboxItem::Kind::Text;
    item.text = j.value("text", "");
    if (j.contains("replyToId")) item.replyToId = j.at("replyToId").get<std::string>();
    item.replyPreview = j.value("replyPreview", "");
    item.replyToSender = j.value("replyToSender", "");
    item.held = j.value("held", false);
    item.replyFromMe = j.value("replyFromMe", false);
    item.mimeType = j.value("mimeType", "");
    item.createdAtMs = j.value("createdAtMs", std::int64_t{0});
    item.order = j.value("order", item.createdAtMs);
    item.attempts = j.value("attempts", 0);
    item.lastError = j.value("lastError", "");
    return item;
}

/// Writes next to the target and renames over it: a crash never leaves a half-written record.
void writeAtomically(const fs::path& target, const std::string& bytes) {
    const fs::path temp = target.string() + ".tmp";
    {
        util::makePrivateFile(temp);
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Couldn't write the outbox");
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) throw std::runtime_error("Couldn't write the outbox");
    }
    fs::rename(temp, target);
}

void removeQuietly(const fs::path& path) {
    std::error_code ignored;
    fs::remove(path, ignored);
}

}  // namespace

crypto::SecretBytes deriveOutboxKey(const crypto::SecretBytes& accountPrivateKey) {
    crypto::ensureSodium();
    if (accountPrivateKey.size() < crypto_kdf_KEYBYTES) throw std::invalid_argument("account private key too short");
    crypto::SecretBytes key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
    if (crypto_kdf_derive_from_key(key.data(), key.size(), kKdfSubkeyId, kKdfContext, accountPrivateKey.data()) != 0) {
        throw crypto::CryptoError("Couldn't derive the outbox key");
    }
    return key;
}

Outbox::Outbox(fs::path directory, crypto::SecretBytes key) : directory_(std::move(directory)), key_(std::move(key)) {
    crypto::ensureSodium();
    if (key_.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) throw std::invalid_argument("outbox key must be 32 bytes");
    util::makePrivateDir(directory_);
}

std::string Outbox::newClientMessageId() {
    crypto::ensureSodium();
    std::uint8_t random[16];
    randombytes_buf(random, sizeof random);
    char encoded[sodium_base64_ENCODED_LEN(sizeof random, sodium_base64_VARIANT_URLSAFE_NO_PADDING)];
    sodium_bin2base64(encoded, sizeof encoded, random, sizeof random, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    return "rv" + std::string(encoded);
}

fs::path Outbox::itemPath(const std::string& id) const {
    if (!safeId(id)) throw std::invalid_argument("bad outbox id");
    return directory_ / (id + ".item");
}

fs::path Outbox::filePath(const std::string& id) const {
    if (!safeId(id)) throw std::invalid_argument("bad outbox id");
    return directory_ / (id + ".file");
}

void Outbox::write(const OutboxItem& item) const {
    const std::string plain = toJson(item).dump();
    const std::string ad = "outbox-item:" + item.id;
    std::string sealed(kMagicSize + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES +
                           plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES,
                       '\0');
    std::copy(kItemMagic, kItemMagic + kMagicSize, sealed.begin());
    auto* nonce = reinterpret_cast<unsigned char*>(sealed.data() + kMagicSize);
    randombytes_buf(nonce, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    unsigned long long written = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char*>(sealed.data() + kMagicSize + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES), &written,
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size(), reinterpret_cast<const unsigned char*>(ad.data()),
        ad.size(), nullptr, nonce, key_.data());
    writeAtomically(itemPath(item.id), sealed);
}

std::optional<OutboxItem> Outbox::read(const fs::path& path) const {
    std::ifstream in(path, std::ios::binary);
    const std::string sealed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t header = kMagicSize + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (sealed.size() < header + crypto_aead_xchacha20poly1305_ietf_ABYTES ||
        sealed.compare(0, kMagicSize, kItemMagic) != 0) {
        return std::nullopt;
    }
    const std::string id = path.stem().string();
    const std::string ad = "outbox-item:" + id;
    std::string plain(sealed.size() - header - crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(plain.data()), &length, nullptr,
            reinterpret_cast<const unsigned char*>(sealed.data() + header), sealed.size() - header,
            reinterpret_cast<const unsigned char*>(ad.data()), ad.size(),
            reinterpret_cast<const unsigned char*>(sealed.data() + kMagicSize), key_.data()) != 0) {
        return std::nullopt;
    }
    plain.resize(length);
    try {
        OutboxItem item = fromJson(Json::parse(plain));
        if (item.id != id) return std::nullopt;
        return item;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

OutboxItem Outbox::addText(const std::string& conversationId, const std::string& text,
                           const std::optional<std::string>& replyToId, const std::string& replyPreview,
                           bool replyFromMe, const std::string& replyToSender) {
    OutboxItem item;
    item.id = newClientMessageId();
    item.conversationId = conversationId;
    item.kind = OutboxItem::Kind::Text;
    item.text = text;
    item.replyToId = replyToId;
    item.replyPreview = replyPreview;
    item.replyFromMe = replyFromMe;
    item.replyToSender = replyToSender;
    item.createdAtMs = nowMs();
    item.order = nextOrder();
    std::lock_guard lock(mutex_);
    write(item);
    return item;
}

OutboxItem Outbox::addFile(const std::string& conversationId, const fs::path& source, const std::string& displayName,
                           const std::string& mimeType) {
    OutboxItem item;
    item.id = newClientMessageId();
    item.conversationId = conversationId;
    item.kind = OutboxItem::Kind::File;
    item.text = displayName;
    item.mimeType = mimeType;
    item.createdAtMs = nowMs();
    item.order = nextOrder();

    std::ifstream in(source, std::ios::binary);
    if (!in) throw std::runtime_error("Couldn't read that file.");
    const fs::path target = filePath(item.id);
    const fs::path temp = target.string() + ".tmp";
    {
        util::makePrivateFile(temp);
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Couldn't write the outbox");
        out.write(kFileMagic, kMagicSize);
        crypto_secretstream_xchacha20poly1305_state state;
        unsigned char header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
        crypto_secretstream_xchacha20poly1305_init_push(&state, header, key_.data());
        out.write(reinterpret_cast<const char*>(header), sizeof header);
        std::vector<unsigned char> plain(kChunk);
        std::vector<unsigned char> sealed(kChunk + crypto_secretstream_xchacha20poly1305_ABYTES);
        const std::string ad = "outbox-file:" + item.id;
        for (;;) {
            in.read(reinterpret_cast<char*>(plain.data()), static_cast<std::streamsize>(plain.size()));
            const auto got = static_cast<std::size_t>(in.gcount());
            if (in.bad()) throw std::runtime_error("Couldn't read that file.");
            const bool last = in.eof();
            unsigned long long length = 0;
            crypto_secretstream_xchacha20poly1305_push(&state, sealed.data(), &length, plain.data(), got,
                                                       reinterpret_cast<const unsigned char*>(ad.data()), ad.size(),
                                                       last ? crypto_secretstream_xchacha20poly1305_TAG_FINAL : 0);
            out.write(reinterpret_cast<const char*>(sealed.data()), static_cast<std::streamsize>(length));
            if (last) break;
        }
        sodium_memzero(plain.data(), plain.size());
        out.flush();
        if (!out) throw std::runtime_error("Couldn't write the outbox");
    }
    std::lock_guard lock(mutex_);
    fs::rename(temp, target);
    write(item);
    return item;
}

void Outbox::openFile(const OutboxItem& item, const fs::path& target) const {
    std::ifstream in(filePath(item.id), std::ios::binary);
    if (!in) throw std::runtime_error("The queued file is missing.");
    char magic[kMagicSize];
    unsigned char header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
    in.read(magic, kMagicSize);
    in.read(reinterpret_cast<char*>(header), sizeof header);
    if (!in || std::string_view(magic, kMagicSize) != std::string_view(kFileMagic, kMagicSize)) {
        throw crypto::CryptoError("The queued file is damaged.");
    }
    crypto_secretstream_xchacha20poly1305_state state;
    if (crypto_secretstream_xchacha20poly1305_init_pull(&state, header, key_.data()) != 0) {
        throw crypto::CryptoError("The queued file is damaged.");
    }
    // This one holds the file in the clear for as long as sending it takes.
    util::makePrivateFile(target);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Couldn't prepare the queued file.");
    std::vector<unsigned char> sealed(kChunk + crypto_secretstream_xchacha20poly1305_ABYTES);
    std::vector<unsigned char> plain(kChunk);
    const std::string ad = "outbox-file:" + item.id;
    bool finished = false;
    while (!finished) {
        in.read(reinterpret_cast<char*>(sealed.data()), static_cast<std::streamsize>(sealed.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;
        unsigned long long length = 0;
        unsigned char tag = 0;
        if (crypto_secretstream_xchacha20poly1305_pull(&state, plain.data(), &length, &tag, sealed.data(), got,
                                                       reinterpret_cast<const unsigned char*>(ad.data()), ad.size()) != 0) {
            out.close();
            removeQuietly(target);
            throw crypto::CryptoError("The queued file is damaged.");
        }
        out.write(reinterpret_cast<const char*>(plain.data()), static_cast<std::streamsize>(length));
        finished = tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL;
    }
    sodium_memzero(plain.data(), plain.size());
    if (!finished) {
        out.close();
        removeQuietly(target);
        throw crypto::CryptoError("The queued file is truncated.");
    }
}

std::vector<OutboxItem> Outbox::items() const {
    std::lock_guard lock(mutex_);
    std::vector<OutboxItem> found;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory_, error)) {
        if (entry.path().extension() != ".item" || !safeId(entry.path().stem().string())) continue;
        if (auto item = read(entry.path())) found.push_back(std::move(*item));
    }
    std::sort(found.begin(), found.end(), [](const OutboxItem& a, const OutboxItem& b) {
        return a.order != b.order ? a.order < b.order : a.id < b.id;
    });
    return found;
}

std::size_t Outbox::count() const {
    return items().size();
}

std::size_t Outbox::countFor(const std::string& conversationId) const {
    const auto all = items();
    return static_cast<std::size_t>(std::count_if(all.begin(), all.end(), [&](const OutboxItem& i) { return i.conversationId == conversationId; }));
}

void Outbox::recordAttempt(const std::string& id, const std::string& error, bool hold) {
    std::lock_guard lock(mutex_);
    auto item = read(itemPath(id));
    if (!item) return;
    item->attempts += 1;
    item->lastError = error;
    item->held = item->held || hold;
    write(*item);
}

void Outbox::release(const std::string& id) {
    std::lock_guard lock(mutex_);
    auto item = read(itemPath(id));
    if (!item) return;
    item->held = false;
    write(*item);
}

void Outbox::remove(const std::string& id) {
    std::lock_guard lock(mutex_);
    removeQuietly(itemPath(id));
    removeQuietly(filePath(id));
}

}  // namespace redvault::data

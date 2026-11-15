// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/data/private_channels.hpp"

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <streambuf>

#include <nlohmann/json.hpp>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/channel_crypto.hpp"
#include "redvault/crypto/channel_e2ee.hpp"
#include "redvault/crypto/file_crypto.hpp"

namespace redvault::data::private_channels {

namespace {

namespace channels = api::channels;
namespace e2ee = crypto::channel_e2ee;
namespace links = crypto::channels;
namespace files = crypto::files;
using Json = nlohmann::json;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::optional<std::string> text(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

template <typename T>
std::optional<T> number(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    return it->get<T>();
}

bool flag(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

std::vector<std::string> strings(const Json& j) {
    std::vector<std::string> out;
    if (!j.is_array()) return out;
    for (const auto& s : j) {
        if (!s.is_string()) throw std::invalid_argument("not a string");
        out.push_back(s.get<std::string>());
    }
    return out;
}

/// One media entry, in the layout the Android app writes too (false and null fields left out).
Json mediaJson(const channels::Media& m) {
    if (!m.secret) throw std::invalid_argument("Media without its key");
    const auto encryptedSize = static_cast<std::int64_t>(files::encryptedSizeFor(static_cast<std::uint64_t>(m.secret->plainSize)));
    Json j{{"id", lower(m.id)},
           {"kind", m.kind},
           {"mime", m.mime},
           {"size", m.secret->plainSize},
           {"fileKey", m.secret->key},
           {"header", m.secret->header},
           {"encryptedSize", encryptedSize},
           {"ciphertextSha256", m.secret->sha256}};
    if (m.filename) j["filename"] = *m.filename;
    if (m.durationMs) j["durationMs"] = *m.durationMs;
    if (m.width) j["width"] = *m.width;
    if (m.height) j["height"] = *m.height;
    return j;
}

std::optional<channels::Media> mediaFrom(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    const auto id = text(j, "id");
    const auto key = text(j, "fileKey");
    const auto header = text(j, "header");
    const auto sha = text(j, "ciphertextSha256");
    const auto size = number<std::int64_t>(j, "size");
    const auto encryptedSize = number<std::int64_t>(j, "encryptedSize");
    if (!id || !key || !header || !sha || !size || *size < 0) return std::nullopt;
    // The blob's size follows from the real size (64 KiB chunks); an entry that says otherwise can't be opened.
    if (encryptedSize && static_cast<std::uint64_t>(*encryptedSize) != files::encryptedSizeFor(static_cast<std::uint64_t>(*size))) return std::nullopt;
    channels::Media m;
    m.id = lower(*id);
    m.kind = text(j, "kind").value_or("file");
    static const std::set<std::string> kKinds{"photo", "video", "file", "audio", "voice", "round"};
    if (!kKinds.contains(m.kind)) m.kind = "file";
    m.mime = text(j, "mime").value_or("application/octet-stream");
    m.size = *size;
    m.filename = text(j, "filename");
    m.durationMs = number<std::int64_t>(j, "durationMs");
    m.width = number<int>(j, "width");
    m.height = number<int>(j, "height");
    m.secret = channels::MediaKey{*key, *header, lower(*sha), *size};
    return m;
}

const channels::Media* entryFor(const std::vector<channels::Media>& entries, const std::string& id) {
    const std::string wanted = lower(id);
    for (const auto& e : entries) {
        if (e.id == wanted) return &e;
    }
    return nullptr;
}

/// Media of an opened post: each server item with its entry from inside (items without one can't be opened and are left out).
std::vector<channels::Media> openedMedia(const std::vector<channels::Media>& fromServer, const std::vector<channels::Media>& entries) {
    std::vector<channels::Media> out;
    for (const auto& m : fromServer) {
        const auto* entry = entryFor(entries, m.id);
        if (!entry) continue;
        channels::Media opened = *entry;
        opened.id = m.id;
        opened.url = m.url;
        out.push_back(std::move(opened));
    }
    return out;
}

bool isApiError(const api::ApiError& e, int status, std::string_view code = {}) {
    return e.status() == status && (code.empty() || e.code() == code);
}

channels::Channel ownChannel(api::ApiClient& api, const std::string& id) {
    for (auto& c : channels::mine(api)) {
        if (lower(c.id) == lower(id)) return c;
    }
    throw api::ApiError(404, "not_found", "This channel doesn't exist, was deleted, or is private.");
}

/// Encrypts a plain stream on the fly in crypto::files' format (64 KiB chunks, the last one FINAL), for an upload body.
class EncryptingInBuf : public std::streambuf {
public:
    EncryptingInBuf(std::istream& plain, std::uint64_t plainSize) : plain_(plain), remaining_(plainSize) {
        crypto::ensureSodium();
        key_ = crypto::SecretBytes(crypto_secretstream_xchacha20poly1305_KEYBYTES);
        header_ = crypto::Bytes(crypto_secretstream_xchacha20poly1305_HEADERBYTES);
        crypto_secretstream_xchacha20poly1305_keygen(key_.data());
        if (crypto_secretstream_xchacha20poly1305_init_push(&state_, header_.data(), key_.data()) != 0) {
            throw crypto::CryptoError("secretstream init failed");
        }
        crypto_hash_sha256_init(&digest_);
        setg(out_.data(), out_.data(), out_.data());
    }
    ~EncryptingInBuf() override { sodium_memzero(&state_, sizeof state_); }

    const crypto::SecretBytes& key() const { return key_; }
    const crypto::Bytes& header() const { return header_; }
    bool finished() const { return finished_; }
    std::string sha256() {
        crypto::Bytes hash(crypto_hash_sha256_BYTES);
        crypto_hash_sha256_final(&digest_, hash.data());
        return crypto::toHex(hash);
    }

protected:
    int_type underflow() override {
        if (finished_) return traits_type::eof();
        const std::size_t len = static_cast<std::size_t>(std::min<std::uint64_t>(files::kChunkBytes, remaining_));
        std::size_t got = 0;
        while (got < len && plain_) {
            plain_.read(reinterpret_cast<char*>(plainChunk_.data()) + got, static_cast<std::streamsize>(len - got));
            got += static_cast<std::size_t>(plain_.gcount());
        }
        if (got != len) throw crypto::CryptoError("The file changed while it was being sent");
        remaining_ -= len;
        const bool last = remaining_ == 0;
        unsigned long long written = 0;
        if (crypto_secretstream_xchacha20poly1305_push(&state_, reinterpret_cast<unsigned char*>(out_.data()), &written, plainChunk_.data(), len,
                                                       nullptr, 0,
                                                       last ? crypto_secretstream_xchacha20poly1305_TAG_FINAL
                                                            : crypto_secretstream_xchacha20poly1305_TAG_MESSAGE) != 0) {
            throw crypto::CryptoError("secretstream push failed");
        }
        crypto_hash_sha256_update(&digest_, reinterpret_cast<const unsigned char*>(out_.data()), written);
        finished_ = last;
        setg(out_.data(), out_.data(), out_.data() + written);
        return traits_type::to_int_type(out_[0]);
    }

private:
    std::istream& plain_;
    std::uint64_t remaining_;
    crypto::SecretBytes key_;
    crypto::Bytes header_;
    crypto_secretstream_xchacha20poly1305_state state_{};
    crypto_hash_sha256_state digest_{};
    crypto::SecretBytes plainChunk_{files::kChunkBytes};
    std::vector<char> out_ = std::vector<char>(files::kChunkBytes + files::kChunkOverheadBytes);
    bool finished_ = false;
};

/// Reads a media link in 1 MiB byte ranges (no cookie), for streaming a large blob into the decryptor.
class RangeInBuf : public std::streambuf {
public:
    RangeInBuf(api::ApiClient& api, std::string url, std::uint64_t size) : api_(api), url_(std::move(url)), size_(size) {}

protected:
    int_type underflow() override {
        if (at_ >= size_) return traits_type::eof();
        const std::uint64_t last = std::min<std::uint64_t>(size_, at_ + kRange) - 1;
        buffer_ = channels::fetchMediaRange(api_, url_, at_, last);
        // A server that ignored the range sent the whole blob.
        if (buffer_.size() > last - at_ + 1) buffer_ = buffer_.substr(static_cast<std::size_t>(at_), static_cast<std::size_t>(last - at_ + 1));
        if (buffer_.empty()) return traits_type::eof();
        at_ += buffer_.size();
        setg(buffer_.data(), buffer_.data(), buffer_.data() + buffer_.size());
        return traits_type::to_int_type(buffer_[0]);
    }

private:
    static constexpr std::uint64_t kRange = 1024 * 1024;
    api::ApiClient& api_;
    std::string url_;
    std::uint64_t size_;
    std::uint64_t at_ = 0;
    std::string buffer_;
};

/// A post this device can't convert (an encrypted one no key opens).
struct Stuck {};

void toEncrypted(api::ApiClient& api, const std::string& channelId, const channels::Post& post, const Keyring& keys) {
    Draft draft;
    draft.content.body = post.body;
    if (post.poll) {
        channels::NewPost::NewPoll poll{post.poll->question, post.poll->options, post.poll->multiple, std::nullopt};
        // The conversion route shows the owner an open quiz's answer too; a server without that fix leaves it unknown.
        if (post.poll->quiz) poll.quiz = channels::NewPost::NewPoll::Quiz{post.poll->correct.value_or(-1), post.poll->explanation.value_or("")};
        draft.content.poll = std::move(poll);
    } else if (post.checklist) {
        channels::NewPost::NewChecklist checklist{post.checklist->title, {}, post.checklist->collaborative};
        for (const auto& item : post.checklist->items) checklist.items.push_back(item.text);
        draft.content.checklist = std::move(checklist);
    }
    for (const auto& m : post.media) {
        const std::string bytes = channels::fetchMedia(api, m.url);
        std::istringstream plain(bytes);
        channels::Media meta = m;
        meta.url.clear();
        meta.secret.reset();
        meta.size = static_cast<std::int64_t>(bytes.size());
        draft.content.media.push_back(uploadEncrypted(
            api, [&](std::int64_t size) { return channels::startEncryptedMedia(api, channelId, size); }, plain, std::move(meta)));
    }
    channels::replacePost(api, channelId, post.id, seal(draft, keys, channelId));
}

void toPlain(api::ApiClient& api, const std::string& channelId, const channels::Post& post, const Keyring& keys) {
    const channels::Post opened = open(post, keys, channelId);
    if (opened.locked) throw Stuck{};
    const auto content = contentOf(post.encrypted->sealed, keys, channelId);
    if (!content) throw Stuck{};
    channels::NewPost plain;
    plain.body = content->body;
    if (content->poll) {
        plain.poll = content->poll;
        // The sealed copy's real answer. Only a quiz sealed without one (converted by a server that hid it) falls back to the first option.
        if (plain.poll->quiz && plain.poll->quiz->correct < 0) plain.poll->quiz->correct = 0;
    } else if (content->checklist) {
        plain.checklist = content->checklist;
    }
    for (const auto& m : opened.media) {
        const std::string bytes = fetchDecrypted(api, m);
        const channels::UploadMeta meta{m.kind, m.mime, static_cast<std::int64_t>(bytes.size()), m.filename, m.durationMs, m.width, m.height};
        const auto started = channels::startMedia(api, channelId, meta);
        channels::uploadMediaBytes(api, started.uploadUrl, bytes);
        plain.media.push_back(started.id);
    }
    channels::replacePost(api, channelId, post.id, plain);
}

}  // namespace

// --- Keyring ---

const crypto::SecretBytes* Keyring::key(int epoch) const {
    const auto it = keys_.find(epoch);
    return it == keys_.end() ? nullptr : &it->second;
}

void Keyring::put(int epoch, crypto::SecretBytes key) {
    keys_.insert_or_assign(epoch, std::move(key));
}

bool Keyring::complete() const {
    const int n = keyEpoch.value_or(0);
    for (int e = 1; e <= n; ++e) {
        if (!key(e)) return false;
    }
    return true;
}

// --- The plaintext ---

std::size_t utf16Length(std::string_view utf8) {
    std::size_t units = 0;
    for (const unsigned char c : utf8) {
        if ((c & 0xC0) == 0x80) continue;  // continuation byte
        units += c >= 0xF0 ? 2 : 1;        // 4-byte sequences are surrogate pairs
    }
    return units;
}

std::string toPlaintext(const Content& content) {
    Json inner = nullptr;
    // False and null fields are left out, as the Android app writes them.
    if (content.poll) {
        Json poll{{"question", content.poll->question}, {"options", content.poll->options}};
        if (content.poll->multiple) poll["multiple"] = true;
        if (content.poll->quiz) {
            Json quiz = Json::object();
            if (content.poll->quiz->correct >= 0) quiz["correct"] = content.poll->quiz->correct;
            if (!content.poll->quiz->explanation.empty()) quiz["explanation"] = content.poll->quiz->explanation;
            poll["quiz"] = quiz;
        }
        inner = Json{{"poll", poll}};
    } else if (content.checklist) {
        Json list{{"title", content.checklist->title}, {"items", content.checklist->items}};
        if (content.checklist->collaborative) list["collaborative"] = true;
        inner = Json{{"checklist", list}};
    } else if (!content.media.empty()) {
        Json list = Json::array();
        for (const auto& m : content.media) list.push_back(mediaJson(m));
        inner = Json{{"media", list}};
    }
    return Json{{"v", 1}, {"body", content.body}, {"content", inner}}.dump();
}

std::optional<Content> parsePlaintext(std::string_view plaintext) {
    try {
        const Json j = Json::parse(plaintext);
        if (!j.is_object() || number<int>(j, "v") != 1) return std::nullopt;
        Content out;
        const auto body = j.find("body");
        if (body == j.end() || !body->is_string()) return std::nullopt;
        out.body = body->get<std::string>();
        const auto inner = j.find("content");
        if (inner == j.end() || inner->is_null()) return out;
        if (!inner->is_object()) return std::nullopt;
        if (const auto it = inner->find("poll"); it != inner->end() && it->is_object()) {
            channels::NewPost::NewPoll poll;
            poll.question = text(*it, "question").value_or("");
            poll.options = strings(it->value("options", Json::array()));
            poll.multiple = flag(*it, "multiple");
            if (const auto q = it->find("quiz"); q != it->end() && q->is_object()) {
                poll.quiz = channels::NewPost::NewPoll::Quiz{number<int>(*q, "correct").value_or(-1), text(*q, "explanation").value_or("")};
            } else if (q != it->end() && q->is_boolean() && q->get<bool>()) {
                poll.quiz = channels::NewPost::NewPoll::Quiz{-1, ""};
            }
            out.poll = std::move(poll);
        } else if (const auto c = inner->find("checklist"); c != inner->end() && c->is_object()) {
            out.checklist = channels::NewPost::NewChecklist{text(*c, "title").value_or(""), strings(c->value("items", Json::array())),
                                                            flag(*c, "collaborative")};
        } else if (const auto m = inner->find("media"); m != inner->end() && m->is_array()) {
            for (const auto& entry : *m) {
                if (auto media = mediaFrom(entry)) out.media.push_back(std::move(*media));
            }
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// --- Keys ---

Keyring unwrapAll(const channels::KeySet& set, std::string_view secret, const std::string& channelId) {
    Keyring out;
    out.keyEpoch = set.keyEpoch;
    crypto::SecretBytes wrapKey;
    try {
        wrapKey = e2ee::wrapKey(secret);
    } catch (const crypto::CryptoError&) {
        return out;
    }
    for (const auto& k : set.keys) {
        if (auto key = e2ee::unwrap(k.wrapped, wrapKey, channelId, k.epoch)) out.put(k.epoch, std::move(*key));
    }
    return out;
}

Keyring readerKeys(api::ApiClient& api, const std::string& channelId, std::string_view secret) {
    const std::string access = links::accessToken(secret);
    return unwrapAll(channels::keys(api, channelId, access), secret, channelId);
}

std::optional<std::string> ownerSecret(const channels::Channel& own, crypto::ByteView publicKey, crypto::ByteView privateKey) {
    return links::openSecret(own.isPrivate ? own.secretSealed : own.formerSecretSealed, publicKey, privateKey);
}

Keyring ownerKeys(api::ApiClient& api, const channels::Channel& own, crypto::ByteView publicKey, crypto::ByteView privateKey) {
    const auto secret = ownerSecret(own, publicKey, privateKey);
    const auto set = channels::ownKeys(api, own.id);
    if (!secret) {
        Keyring none;
        none.keyEpoch = set.keyEpoch;
        return none;
    }
    return unwrapAll(set, *secret, own.id);
}

// --- Opening ---

std::optional<Content> contentOf(const channels::SealedBody& sealed, const Keyring& keys, const std::string& channelId) {
    const auto* key = keys.key(sealed.keyEpoch);
    if (!key) return std::nullopt;
    const auto plaintext = e2ee::decrypt(sealed.nonce, sealed.ciphertext, *key, channelId, sealed.keyEpoch);
    if (!plaintext) return std::nullopt;
    return parsePlaintext(*plaintext);
}

channels::Post open(channels::Post post, const Keyring& keys, const std::string& channelId) {
    if (!post.encrypted) return post;
    const auto content = contentOf(post.encrypted->sealed, keys, channelId);
    post.poll.reset();
    post.checklist.reset();
    if (!content) {
        post.locked = true;
        post.body.clear();
        post.media.clear();
        return post;
    }
    post.locked = false;
    post.body = content->body;
    const auto& enc = *post.encrypted;
    if (enc.poll) {
        channels::Poll p;
        if (content->poll) {
            p.question = content->poll->question;
            p.options = content->poll->options;
            if (content->poll->quiz && content->poll->quiz->correct >= 0) {
                p.sealedCorrect = content->poll->quiz->correct;
                if (!content->poll->quiz->explanation.empty()) p.sealedExplanation = content->poll->quiz->explanation;
            }
        }
        p.options.resize(static_cast<std::size_t>(std::max(0, enc.poll->optionCount)));
        p.multiple = enc.poll->multiple;
        p.quiz = enc.poll->quiz;
        p.closed = enc.poll->closed;
        p.counts = enc.poll->counts;
        p.counts.resize(p.options.size(), 0);
        p.voters = enc.poll->voters;
        if (p.quiz && p.closed) {
            p.correct = p.sealedCorrect;
            p.explanation = p.sealedExplanation;
        }
        post.poll = std::move(p);
    } else if (enc.checklist) {
        channels::Checklist c;
        c.title = content->checklist ? content->checklist->title : std::string();
        c.collaborative = enc.checklist->collaborative;
        for (std::size_t i = 0; i < static_cast<std::size_t>(std::max(0, enc.checklist->itemCount)); ++i) {
            const std::string itemText = content->checklist && i < content->checklist->items.size() ? content->checklist->items[i] : std::string();
            c.items.push_back(channels::ChecklistItem{itemText, i < enc.checklist->done.size() && enc.checklist->done[i]});
        }
        post.checklist = std::move(c);
    }
    post.media = openedMedia(post.media, content->media);
    return post;
}

std::vector<channels::Post> open(std::vector<channels::Post> posts, const Keyring& keys, const std::string& channelId) {
    for (auto& p : posts) p = open(std::move(p), keys, channelId);
    return posts;
}

channels::Suggestion open(channels::Suggestion suggestion, const Keyring& keys, const std::string& channelId) {
    if (!suggestion.encrypted) return suggestion;
    const auto content = contentOf(*suggestion.encrypted, keys, channelId);
    if (!content) {
        suggestion.locked = true;
        suggestion.body.clear();
        suggestion.media.clear();
        return suggestion;
    }
    suggestion.locked = false;
    suggestion.body = content->body;
    suggestion.media = openedMedia(suggestion.media, content->media);
    return suggestion;
}

channels::Poll mergePoll(const channels::Poll& opened, const channels::Poll& fromServer, bool answered) {
    // A plain poll's answer has its own texts.
    if (!fromServer.question.empty()) return fromServer;
    channels::Poll out = opened;
    out.closed = fromServer.closed;
    out.counts = fromServer.counts;
    out.counts.resize(out.options.size(), 0);
    out.voters = fromServer.voters;
    if (out.quiz && (answered || out.closed) && out.sealedCorrect) {
        out.correct = out.sealedCorrect;
        out.explanation = out.sealedExplanation;
    }
    return out;
}

channels::Checklist mergeChecklist(const channels::Checklist& opened, const channels::Checklist& fromServer) {
    if (!fromServer.title.empty()) return fromServer;
    channels::Checklist out = opened;
    for (std::size_t i = 0; i < out.items.size(); ++i) out.items[i].done = i < fromServer.items.size() && fromServer.items[i].done;
    return out;
}

// --- Sealing ---

namespace {

channels::SealedBody sealContent(const Content& content, const Keyring& keys, const std::string& channelId) {
    if (!keys.canSeal()) throw api::UserFacingError("This device doesn't have the channel's newest key. Reload the channel and try again.");
    if (utf16Length(content.body) > static_cast<std::size_t>(channels::kMaxPostLength)) {
        throw api::UserFacingError("A post is at most 4000 characters.");
    }
    const int epoch = *keys.keyEpoch;
    try {
        const auto sealed = e2ee::encrypt(toPlaintext(content), *keys.key(epoch), channelId, epoch);
        return channels::SealedBody{epoch, sealed.nonce, sealed.ciphertext};
    } catch (const crypto::CryptoError&) {
        throw api::UserFacingError("This post is too long to send.");
    }
}

}  // namespace

channels::NewEncryptedPost seal(const Draft& draft, const Keyring& keys, const std::string& channelId) {
    channels::NewEncryptedPost out;
    out.sealed = sealContent(draft.content, keys, channelId);
    if (draft.content.poll) {
        out.poll = channels::NewEncryptedPost::Poll{static_cast<int>(draft.content.poll->options.size()), draft.content.poll->multiple,
                                                    draft.content.poll->quiz.has_value()};
    } else if (draft.content.checklist) {
        out.checklist = channels::NewEncryptedPost::Checklist{static_cast<int>(draft.content.checklist->items.size()),
                                                              draft.content.checklist->collaborative};
    } else {
        for (const auto& m : draft.content.media) out.media.push_back(m.id);
    }
    out.publishAt = draft.publishAt;
    return out;
}

channels::SealedBody sealSuggestion(const Content& content, const Keyring& keys, const std::string& channelId) {
    return sealContent(content, keys, channelId);
}

channels::Post send(api::ApiClient& api, const std::string& channelId, const Draft& draft, KeyringPtr& keys, const Refetch& refetch,
                    const std::optional<std::string>& postId) {
    for (int attempt = 0;; ++attempt) {
        if (!keys) throw api::UserFacingError("This device doesn't have the channel's key. Reload the channel and try again.");
        const auto sealed = seal(draft, *keys, channelId);
        try {
            return postId ? channels::replacePost(api, channelId, *postId, sealed) : channels::post(api, channelId, sealed);
        } catch (const api::ApiError& e) {
            if (attempt > 0 || !refetch || !isApiError(e, 409, "stale_key_epoch")) throw;
            keys = refetch();
        }
    }
}

// --- Media ---

channels::Media uploadEncrypted(api::ApiClient& api, const StartEncrypted& start, std::istream& plain, channels::Media meta,
                                const Progress& onProgress) {
    if (meta.size < 0) throw std::invalid_argument("Negative size");
    const std::uint64_t plainSize = static_cast<std::uint64_t>(meta.size);
    const std::uint64_t blobSize = files::encryptedSizeFor(plainSize);
    const auto started = start(static_cast<std::int64_t>(blobSize));
    static const std::string kPrefix = "/api/";
    if (!started.uploadUrl.starts_with(kPrefix)) throw api::ApiError(0, "malformed_response", "Unexpected upload URL");
    EncryptingInBuf encrypting(plain, plainSize);
    struct Counting : std::streambuf {
        Counting(std::streambuf* source, const Progress& progress) : source_(source), progress_(progress) {}
        int_type underflow() override {
            const std::streamsize n = source_->sgetn(buffer_, sizeof buffer_);
            if (n <= 0) return traits_type::eof();
            count_ += n;
            if (progress_) progress_(count_);
            setg(buffer_, buffer_, buffer_ + n);
            return traits_type::to_int_type(buffer_[0]);
        }
        std::streambuf* source_;
        const Progress& progress_;
        char buffer_[64 * 1024];
        std::int64_t count_ = 0;
    } counting(&encrypting, onProgress);
    std::istream counted(&counting);
    // Only the shape the server hands out (as uploadMedia()), so the bytes and the cookie can't go anywhere else.
    static const std::string kUploads = "/api/channel-uploads/";
    if (!started.uploadUrl.starts_with(kUploads) || started.uploadUrl.size() != kUploads.size() + 36) {
        throw api::ApiError(0, "malformed_response", "Unexpected upload URL");
    }
    api.callUpload("PUT", std::string_view(started.uploadUrl).substr(kPrefix.size()), counted, blobSize, "application/octet-stream");
    if (!encrypting.finished()) throw crypto::CryptoError("The upload ended early");
    meta.id = lower(started.id);
    meta.url.clear();
    meta.secret = channels::MediaKey{crypto::base64Encode(encrypting.key()), crypto::base64Encode(encrypting.header()), encrypting.sha256(),
                                     meta.size};
    return meta;
}

std::string fetchDecrypted(api::ApiClient& api, const channels::Media& media) {
    if (!media.secret) throw files::FileDecryptError();
    const std::string blob = channels::fetchMedia(api, media.url);
    std::istringstream in(blob);
    std::ostringstream out;
    const auto key = crypto::base64Decode(media.secret->key);
    const auto header = crypto::base64Decode(media.secret->header);
    files::decrypt(in, out, key, header, files::encryptedSizeFor(static_cast<std::uint64_t>(media.secret->plainSize)), media.secret->sha256);
    return std::move(out).str();
}

void downloadDecrypted(api::ApiClient& api, const channels::Media& media, std::ostream& out) {
    if (!media.secret) throw files::FileDecryptError();
    const std::uint64_t blobSize = files::encryptedSizeFor(static_cast<std::uint64_t>(media.secret->plainSize));
    RangeInBuf ranges(api, media.url, blobSize);
    std::istream in(&ranges);
    const auto key = crypto::base64Decode(media.secret->key);
    const auto header = crypto::base64Decode(media.secret->header);
    files::decrypt(in, out, key, header, blobSize, media.secret->sha256);
}

// --- The owner switching visibility ---

bool needsKeys(const channels::Channel& own) {
    return own.isPrivate && !own.keyEpoch;
}

Switched makePrivate(api::ApiClient& api, const channels::Channel& own, crypto::ByteView publicKey, crypto::ByteView privateKey) {
    channels::Channel current = own;
    for (int attempt = 0;; ++attempt) {
        const auto set = channels::ownKeys(api, current.id);
        const int n = set.keyEpoch.value_or(0);
        Keyring old;
        if (n > 0) {
            const auto secret = ownerSecret(current, publicKey, privateKey);
            if (secret) old = unwrapAll(set, *secret, current.id);
            old.keyEpoch = n;
            if (!old.complete()) {
                throw api::UserFacingError(
                    "This device can't open the channel's older keys, so a new link wouldn't open the older posts. Try from the device that made the "
                    "link.");
            }
        }
        const std::string secret = links::newSecret();
        const auto wrapKey = e2ee::wrapKey(secret);
        auto fresh = e2ee::newContentKey();
        std::vector<channels::WrappedKey> wrapped;
        for (int e = 1; e <= n; ++e) wrapped.push_back(channels::WrappedKey{e, e2ee::wrap(*old.key(e), wrapKey, current.id, e)});
        wrapped.push_back(channels::WrappedKey{n + 1, e2ee::wrap(fresh, wrapKey, current.id, n + 1)});
        try {
            auto updated = channels::setPrivate(api, current.id, links::accessToken(secret), links::sealSecret(secret, publicKey), wrapped);
            auto keys = std::make_shared<Keyring>(std::move(old));
            keys->put(n + 1, std::move(fresh));
            keys->keyEpoch = updated.keyEpoch.value_or(n + 1);
            return Switched{std::move(updated), secret, std::move(keys)};
        } catch (const api::ApiError& e) {
            // Another device reset the link meanwhile: start again from what it made.
            if (attempt > 0 || !isApiError(e, 409, "wrong_key_epochs")) throw;
            current = ownChannel(api, current.id);
        }
    }
}

Switched setUpKeys(api::ApiClient& api, const channels::Channel& own, crypto::ByteView publicKey, crypto::ByteView privateKey) {
    if (!own.isPrivate || !own.secretSealed) throw api::UserFacingError("This channel isn't private.");
    const auto secret = links::openSecret(own.secretSealed, publicKey, privateKey);
    if (!secret) throw api::UserFacingError("This device can't open the channel's private link.");
    if (own.keyEpoch) return Switched{own, *secret, std::make_shared<Keyring>(ownerKeys(api, own, publicKey, privateKey))};
    const auto wrapKey = e2ee::wrapKey(*secret);
    auto fresh = e2ee::newContentKey();
    const std::vector<channels::WrappedKey> wrapped{channels::WrappedKey{1, e2ee::wrap(fresh, wrapKey, own.id, 1)}};
    try {
        // The same link: followers keep reading.
        auto updated = channels::setPrivate(api, own.id, links::accessToken(*secret), *own.secretSealed, wrapped);
        auto keys = std::make_shared<Keyring>();
        keys->put(1, std::move(fresh));
        keys->keyEpoch = updated.keyEpoch.value_or(1);
        return Switched{std::move(updated), *secret, std::move(keys)};
    } catch (const api::ApiError& e) {
        // Another of the owner's devices did it first: use its keys.
        if (!isApiError(e, 409, "wrong_key_epochs")) throw;
        const auto now = ownChannel(api, own.id);
        const auto nowSecret = ownerSecret(now, publicKey, privateKey);
        return Switched{now, nowSecret.value_or(*secret), std::make_shared<Keyring>(ownerKeys(api, now, publicKey, privateKey))};
    }
}

ConversionResult convert(api::ApiClient& api, const std::string& channelId, const Keyring& keys, const ConversionHooks& hooks) {
    ConversionResult result;
    std::set<std::string> stuck;
    int attempt = 0;
    const auto cancelled = [&] { return hooks.cancelled && hooks.cancelled(); };
    // Rate limited: wait and try again, at most kMaxWaits times in a row; then stop, to resume next time.
    constexpr int kMaxWaits = 5;
    const auto backOff = [&](const api::ApiError& e) {
        if (++attempt > kMaxWaits) throw e;
        if (hooks.backOff) hooks.backOff(attempt);
    };
    while (!cancelled()) {
        channels::Conversion page;
        try {
            page = channels::conversion(api, channelId, 50);
        } catch (const api::ApiError& e) {
            if (e.status() != 429) throw;
            backOff(e);
            continue;
        }
        std::vector<channels::Post> todo;
        for (auto& p : page.posts) {
            if (!stuck.contains(p.id)) todo.push_back(std::move(p));
        }
        if (todo.empty()) {
            result.finished = page.posts.empty();
            result.stuck = static_cast<int>(stuck.size());
            return result;
        }
        for (const auto& post : todo) {
            while (true) {
                if (cancelled()) return result;
                try {
                    if (page.isPrivate) {
                        if (post.encrypted) break;  // already converted meanwhile
                        toEncrypted(api, channelId, post, keys);
                    } else {
                        if (!post.encrypted) break;
                        toPlain(api, channelId, post, keys);
                    }
                    ++result.converted;
                    attempt = 0;
                    if (hooks.progress) hooks.progress(result.converted);
                    break;
                } catch (const api::ApiError& e) {
                    if (e.status() != 429) throw;
                    backOff(e);
                } catch (const Stuck&) {
                    stuck.insert(post.id);
                    break;
                }
            }
        }
    }
    result.stuck = static_cast<int>(stuck.size());
    return result;
}

}  // namespace redvault::data::private_channels

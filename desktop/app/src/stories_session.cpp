// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "stories_session.hpp"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QStandardPaths>

#include <algorithm>
#include <sstream>
#include <utility>

#include "chat_images.hpp"
#include "chat_session.hpp"
#include "prefs.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/file_crypto.hpp"
#include "redvault/crypto/story_crypto.hpp"
#include "redvault/model/rich_message.hpp"
#include "redvault/util/text.hpp"

namespace redvault::app {

namespace st = model::stories;
namespace sapi = api::stories;
namespace sc = crypto::stories;

namespace stories_detail {

struct Decoded {
    sapi::Story dto;
    st::Meta meta;
    std::shared_ptr<const crypto::SecretBytes> key;
    std::int64_t createdMs = 0;
    bool seen = false;
    int viewCount = 0;
};

struct PrefsResult {
    st::Prefs prefs;
    bool archive = false;
    int revision = 0;
};

/// One answer of yours, as far as it can be read back. Answers sent without a name can't be, even by their sender.
struct MyAnswer {
    bool readable = false;
    bool anonymous = false;
    st::Answer answer;
};

struct InteractionsResult {
    std::string storyId;
    bool own = false;
    std::map<std::string, MyAnswer> mine;
    std::map<std::string, st::Totals> totals;
    /// Your own story: the answers sealed to you that could be opened.
    std::vector<st::InboxEntry> entries;
    /// Answers that could not be opened or didn't fit their sticker (never counted).
    int unreadable = 0;
    /// `storyId:stickerId` -> the totals now published.
    std::map<std::string, std::string> published;
};

struct FeedResult {
    std::map<std::string, Friend> friends;
    std::vector<std::shared_ptr<Decoded>> own;
    std::vector<std::pair<std::string, std::vector<std::shared_ptr<Decoded>>>> authors;
    std::optional<sapi::Config> config;
    std::optional<PrefsResult> prefs;
    int generation = 0;
};

}  // namespace stories_detail

namespace {

using stories_detail::Decoded;
using stories_detail::Friend;
using DecodedPtr = std::shared_ptr<Decoded>;

constexpr const char* kSeenKey = "storiesSeen";
constexpr std::int64_t kGapFillEveryMs = 5 * 60 * 1000;
constexpr int kMaxPrefsAttempts = 3;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

bool has(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

/// The server's own words for an API error, or `fallback` for anything else.
QString worded(const Failure& failure, const QString& fallback) {
    return failure.code.isEmpty() ? fallback : failure.message;
}

QString mediaDir() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/story-media");
}

/// Where this computer keeps unfinished stories: sealed, one folder per account.
QString draftsDir() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/story-drafts");
}

QString imageKey(const std::string& storyId) {
    return QStringLiteral("story-") + qs(storyId);
}

st::Overlay overlayFrom(const QVariantMap& m) {
    st::Overlay o;
    o.type = m.value("type").toString().toStdString();
    o.text = m.value("text").toString().toStdString();
    o.x = m.value("x").toDouble();
    o.y = m.value("y").toDouble();
    o.scale = m.value("scale", 1.0).toDouble();
    o.rotation = m.value("rotation", 0.0).toDouble();
    o.color = m.value("color", QStringLiteral("white")).toString().toStdString();
    o.boxed = m.value("boxed").toBool();
    o.font = m.value("font", QStringLiteral("classic")).toString().toStdString();
    return o;
}

/// Who your chats say each friend is: their key in your 1:1 chat with them.
std::map<std::string, std::string> chatKeysOf(ChatSession* chats) {
    std::map<std::string, std::string> out;
    if (!chats) return out;
    for (const auto& meta : chats->metas()) {
        if (meta && !meta->isGroup && !meta->otherUserId.empty()) out[meta->otherUserId] = meta->otherPublicKeyB64;
    }
    return out;
}

/// Opens one story; null if it can't be (not made by its author for you, or malformed).
DecodedPtr decodeStory(const sapi::Story& dto, crypto::ByteView authorKey, crypto::ByteView viewerKey) {
    try {
        auto key = std::make_shared<const crypto::SecretBytes>(sc::unwrapStoryKey(dto.id, crypto::base64Decode(dto.wrappedKey), authorKey, viewerKey));
        const std::string json = sc::openMeta(dto.id, *key, crypto::base64Decode(dto.metaCiphertext), crypto::base64Decode(dto.metaNonce));
        auto d = std::make_shared<Decoded>();
        d->meta = st::parseMeta(json, dto.kind);
        d->dto = dto;
        d->key = std::move(key);
        d->createdMs = util::parseIsoMillis(dto.createdAt).value_or(0);
        d->seen = dto.seen;
        d->viewCount = dto.viewCount;
        return d;
    } catch (const std::exception&) {
        return nullptr;
    }
}

/// Friends with the key their stories are sealed with; one whose chat shows a different key is left out.
std::map<std::string, Friend> loadFriends(api::ApiClient& api, const std::map<std::string, std::string>& chatKeys) {
    std::map<std::string, Friend> out;
    for (const auto& f : api.listFriends()) {
        const auto chat = chatKeys.find(f.userId);
        if (chat != chatKeys.end() && !chat->second.empty() && chat->second != f.publicKey) continue;
        try {
            out[f.userId] = Friend{f.userId, f.username, f.avatarUrl, crypto::base64Decode(f.publicKey)};
        } catch (const crypto::CryptoError&) {
        }
    }
    return out;
}

stories_detail::PrefsResult loadPrefs(api::ApiClient& api, const data::SelfIdentity& me) {
    const auto dto = sapi::prefs(api);
    stories_detail::PrefsResult out{{}, dto.archiveEnabled, dto.revision};
    if (dto.prefsCiphertext && dto.prefsNonce) {
        try {
            out.prefs = st::parsePrefs(sc::openPrefs(crypto::base64Decode(*dto.prefsCiphertext), crypto::base64Decode(*dto.prefsNonce), me.privateKey));
        } catch (const std::exception&) {
            // Unreadable (made with an older key?): start from the defaults.
        }
    }
    return out;
}

std::string wrapFor(const std::string& storyId, const crypto::SecretBytes& key, crypto::ByteView publicKey, const data::SelfIdentity& me) {
    return crypto::base64Encode(sc::wrapStoryKey(storyId, key, publicKey, me.privateKey));
}

/**
 * Your "friends" stories (live, or kept in a highlight) and friends with no
 * key yet: seal them one, except anyone you hide stories from or whose key
 * the server reports differently from the one this app knows.
 */
void fillKeyGaps(api::ApiClient& api, const data::SelfIdentity& me, const std::vector<DecodedPtr>& own, const std::map<std::string, Friend>& friends,
                 const st::Prefs& prefs) {
    const auto gaps = sapi::keyGaps(api);
    if (gaps.empty()) return;
    std::map<std::string, std::shared_ptr<const crypto::SecretBytes>> keys;
    for (const auto& d : own) keys[d->dto.id] = d->key;
    const bool missing = std::any_of(gaps.begin(), gaps.end(), [&](const sapi::KeyGap& g) { return !keys.count(g.storyId); });
    if (missing) {
        // Highlighted stories past their 24 hours are in the archive.
        for (const auto& dto : sapi::archive(api)) {
            if (auto d = decodeStory(dto, me.publicKey, me.privateKey)) keys[d->dto.id] = d->key;
        }
    }
    for (const auto& gap : gaps) {
        const auto key = keys.find(gap.storyId);
        if (key == keys.end()) continue;
        std::vector<sapi::WrappedKey> wrapped;
        for (const auto& [userId, publicKey] : gap.users) {
            if (has(prefs.hideFrom, userId)) continue;
            const auto f = friends.find(userId);
            if (f == friends.end() || crypto::base64Encode(f->second.publicKey) != publicKey) continue;
            wrapped.push_back({userId, wrapFor(gap.storyId, *key->second, f->second.publicKey, me)});
        }
        if (!wrapped.empty()) sapi::addKeys(api, gap.storyId, wrapped);
    }
}

/// The host of a link that safeStoryLink already checked: "https://host[:port]/...".
std::string hostOf(const std::string& url) {
    constexpr std::size_t kStart = 8;  // "https://"
    if (url.size() <= kStart) return {};
    const std::size_t end = url.find_first_of("/:?#", kStart);
    return url.substr(kStart, end == std::string::npos ? std::string::npos : end - kStart);
}

/// A sticker from the fields of a sticker map (what stickerMap writes and the composer edits). Not yet checked.
st::Sticker stickerFromMap(const QVariantMap& m) {
    st::Sticker s;
    s.type = m.value("type").toString().toStdString();
    s.id = m.value("id").toString().toStdString();
    s.x = m.value("x", 0.5).toDouble();
    s.y = m.value("y", 0.5).toDouble();
    s.scale = m.value("scale", 1.0).toDouble();
    s.rotation = m.value("rotation", 0.0).toDouble();
    s.text = m.value("text").toString().toStdString();
    for (const auto& o : m.value("options").toList()) s.options.push_back(o.toString().toStdString());
    s.correct = m.value("correct").toInt();
    s.explanation = m.value("explanation").toString().toStdString();
    s.resultsEveryone = m.value("resultsEveryone", true).toBool();
    if (const double closes = m.value("closesAt").toDouble(); closes > 0) s.closesAt = closes;
    s.anonymous = m.value("anonymous").toBool();
    s.emoji = m.value("emoji").toString().toStdString();
    s.when = m.value("when").toDouble();
    s.style = m.value("style").toString().toStdString();
    s.url = m.value("url").toString().toStdString();
    s.userId = util::toLowerAscii(m.value("userId").toString().toStdString());
    return s;
}

struct PreparedMedia {
    std::string kind;  // "image" | "video"
    st::MediaRef media;
    std::string bytes;
};

/// The photo or video as a story sends it. A photo is redrawn and saved fresh as JPEG, so its location, camera and time
/// stay behind; a video goes as it is. Throws std::runtime_error with a sentence for the person.
PreparedMedia prepareMedia(const QString& path, bool video, int durationMs) {
    PreparedMedia out;
    if (video) {
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly)) throw std::runtime_error("That video couldn't be opened.");
        const QByteArray all = in.readAll();
        out.bytes.assign(all.constData(), static_cast<std::size_t>(all.size()));
        const QString ext = QFileInfo(path).suffix().toLower();
        out.media.mime = ext == "webm" ? "video/webm" : ext == "mov" ? "video/quicktime" : "video/mp4";
        out.media.width = 1080;
        out.media.height = 1920;
        out.media.durationMs = std::min<std::int64_t>(durationMs, st::kMaxVideoMs);
        out.kind = "video";
        return out;
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) throw std::runtime_error("That photo couldn't be opened.");
    if (std::max(image.width(), image.height()) > st::kMaxImageSide) {
        image = image.scaled(st::kMaxImageSide, st::kMaxImageSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    if (!image.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPEG", 90)) throw std::runtime_error("That photo couldn't be prepared.");
    out.bytes.assign(jpeg.constData(), static_cast<std::size_t>(jpeg.size()));
    out.media.mime = "image/jpeg";
    out.media.width = image.width();
    out.media.height = image.height();
    out.kind = "image";
    return out;
}

/// A fresh sticker id (a-z0-9, 12 characters): the server only ever sees this.
std::string newStickerId() {
    static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    const auto random = crypto::randomBytes(12);
    std::string id;
    for (const auto b : random) id += kAlphabet[b % 36];
    return id;
}

QVariantMap mineMap(const stories_detail::MyAnswer& a) {
    QVariantMap m{{"answered", true}, {"readable", a.readable}, {"anonymous", a.anonymous}, {"kind", qs(a.answer.kind)}};
    if (a.readable) {
        m["choice"] = a.answer.choice;
        m["value"] = a.answer.value;
        m["text"] = qs(a.answer.text);
    }
    return m;
}

QVariantMap totalsMap(const st::Totals& t) {
    QVariantList counts;
    for (const int c : t.counts) counts.append(c);
    return {{"total", t.total},
            {"counts", counts},
            {"hasCorrect", t.correct.has_value()},
            {"correct", t.correct.value_or(0)},
            {"hasAverage", t.average.has_value()},
            {"average", t.average.value_or(0.0)}};
}

/// The words on a story a reporter may choose to include: what they can read, nothing else (never a photo or video). At most 600 units.
QString evidenceOf(const st::Meta& m) {
    QStringList parts;
    if (!m.text.empty()) parts << qs(m.text);
    for (const auto& o : m.overlays) {
        if (o.type == "text") parts << qs(o.text);
    }
    for (const auto& s : m.stickers) {
        if (s.type == "poll" || s.type == "quiz") {
            QStringList options;
            for (const auto& o : s.options) options << qs(o);
            parts << qs(s.text) + QStringLiteral(" [") + options.join(QStringLiteral(" / ")) + QStringLiteral("]");
        } else if (s.type == "question" || s.type == "slider" || s.type == "countdown" || s.type == "location") {
            parts << qs(s.text);
        } else if (s.type == "link") {
            parts << qs(s.text) + u' ' + qs(s.url);
        }
    }
    return parts.join(QStringLiteral(" | ")).left(600);
}

QVariantMap overlayMap(const st::Overlay& o) {
    return {{"type", qs(o.type)},   {"text", qs(o.text)},   {"x", o.x},         {"y", o.y},          {"scale", o.scale},
            {"rotation", o.rotation}, {"color", qs(o.color)}, {"boxed", o.boxed}, {"font", qs(o.font)}};
}

QStringList qList(const std::vector<std::string>& list) {
    QStringList out;
    for (const auto& s : list) out.append(qs(s));
    return out;
}

/// "friends", "close_friends" or "custom": anything else is a story for your friends.
std::string audienceOf(const QString& audience) {
    return audience == QStringLiteral("close_friends") ? "close_friends" : audience == QStringLiteral("custom") ? "custom" : "friends";
}

std::vector<std::string> stdList(const QStringList& list) {
    std::vector<std::string> out;
    for (const auto& s : list) {
        const auto id = util::toLowerAscii(s.toStdString());
        if (!has(out, id)) out.push_back(id);
    }
    return out;
}

}  // namespace

StoriesSession::StoriesSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, ChatSession* chats,
                               QObject* parent)
    : QObject(parent), api_(std::move(api)), identity_(std::move(identity)), chats_(chats) {
    pool_.setMaxThreadCount(3);
    // Videos left from a session that didn't close cleanly.
    QDir(mediaDir()).removeRecursively();
    draftStore_ = std::make_unique<data::StoryDraftStore>(std::filesystem::path(draftsDir().toStdU16String()));
    refresh();
    refreshDrafts();
}

StoriesSession::~StoriesSession() {
    pool_.clear();
    pool_.waitForDone();
    dropTempFiles();
    dropDraftFiles();
}

QStringList StoriesSession::colors() const {
    return qList(st::kColors);
}

QStringList StoriesSession::backgrounds() const {
    return qList(st::kBackgrounds);
}

QStringList StoriesSession::fonts() const {
    return qList(st::kFonts);
}

std::shared_ptr<StoriesSession::Decoded> StoriesSession::find(const std::string& storyId) const {
    const auto it = known_.find(storyId);
    return it == known_.end() ? nullptr : it->second;
}

QVariantMap StoriesSession::stickerMap(const st::Sticker& s) const {
    QVariantList options;
    for (const auto& o : s.options) options.append(qs(o));
    QString handle;
    if (s.type == "mention") {
        const auto f = friends_.find(s.userId);
        handle = s.userId == identity_->userId ? qs(identity_->handle) : f == friends_.end() ? QStringLiteral("someone") : qs(f->second.handle);
    }
    return {{"type", qs(s.type)},
            {"id", qs(s.id)},
            {"x", s.x},
            {"y", s.y},
            {"scale", s.scale},
            {"rotation", s.rotation},
            {"text", qs(s.text)},
            {"options", options},
            {"correct", s.correct},
            {"explanation", qs(s.explanation)},
            {"resultsEveryone", s.resultsEveryone},
            {"closesAt", s.closesAt.value_or(0)},
            {"anonymous", s.anonymous},
            {"emoji", qs(s.emoji)},
            {"when", s.when},
            {"style", qs(s.style)},
            {"url", qs(s.url)},
            {"host", qs(hostOf(s.url))},
            {"userId", qs(s.userId)},
            {"handle", handle},
            {"answerable", st::isAnswerable(s)}};
}

QVariantMap StoriesSession::storyMap(const Decoded& d) const {
    QVariantList overlays;
    for (const auto& o : d.meta.overlays) overlays.append(overlayMap(o));
    QVariantList stickers;
    bool answerable = false;
    for (const auto& s : d.meta.stickers) {
        stickers.append(stickerMap(s));
        answerable = answerable || st::isAnswerable(s);
    }
    return {{"stickers", stickers},
            {"answerable", answerable},
            {"expiresAt", static_cast<double>(util::parseIsoMillis(d.dto.expiresAt).value_or(0))},
            {"id", qs(d.dto.id)},
            {"authorId", qs(d.dto.authorId)},
            {"kind", qs(d.meta.kind)},
            {"text", qs(d.meta.text)},
            {"background", qs(d.meta.background)},
            {"font", qs(d.meta.font)},
            {"overlays", overlays},
            {"closeFriends", d.meta.closeFriends},
            {"age", qs(st::age(d.createdMs, util::nowMillis()))},
            {"allowReplies", d.dto.allowReplies},
            {"viewCount", d.viewCount},
            {"hasMedia", d.meta.media.has_value()},
            {"durationMs", d.meta.media && d.meta.media->durationMs ? static_cast<double>(*d.meta.media->durationMs) : 0.0},
            {"preview", qs(st::preview(d.meta))},
            {"seen", d.seen},
            {"createdAt", static_cast<double>(d.createdMs)}};
}

QVariantMap StoriesSession::ownerMap(const std::string& userId) const {
    if (userId == identity_->userId) {
        return {{"userId", qs(userId)}, {"handle", qs(identity_->handle)}, {"avatarUrl", qs(identity_->avatarUrl.value_or(""))}, {"isSelf", true}};
    }
    const auto f = friends_.find(userId);
    return {{"userId", qs(userId)},
            {"handle", f == friends_.end() ? QStringLiteral("Someone") : qs(f->second.handle)},
            {"avatarUrl", f == friends_.end() ? QString() : qs(f->second.avatarUrl.value_or(""))},
            {"isSelf", false}};
}

QVariantList StoriesSession::tray() const {
    QVariantList out;
    for (const auto& [authorId, stories] : authors_) {
        if (has(prefs_.muted, authorId)) continue;
        int unseen = 0;
        bool close = false;
        for (const auto& d : stories) {
            if (d->seen) continue;
            ++unseen;
            close = close || d->meta.closeFriends;
        }
        QVariantMap row = ownerMap(authorId);
        row["authorId"] = qs(authorId);
        row["unseen"] = unseen;
        row["ring"] = unseen == 0 ? QStringLiteral("seen") : close ? QStringLiteral("close") : QStringLiteral("new");
        out.append(row);
    }
    return out;
}

QVariantList StoriesSession::friends() const {
    std::vector<const Friend*> sorted;
    for (const auto& [id, f] : friends_) sorted.push_back(&f);
    std::sort(sorted.begin(), sorted.end(), [](const Friend* a, const Friend* b) { return util::toLowerAscii(a->handle) < util::toLowerAscii(b->handle); });
    QVariantList out;
    for (const auto* f : sorted) {
        out.append(QVariantMap{{"userId", qs(f->userId)},
                               {"handle", qs(f->handle)},
                               {"avatarUrl", qs(f->avatarUrl.value_or(""))},
                               {"close", has(prefs_.closeFriends, f->userId)},
                               {"hidden", has(prefs_.hideFrom, f->userId)},
                               {"muted", has(prefs_.muted, f->userId)},
                               {"isFriend", true}});
    }
    // Muted people who aren't friends any more still need an Unmute.
    for (const auto& id : prefs_.muted) {
        if (friends_.count(id)) continue;
        out.append(QVariantMap{{"userId", qs(id)}, {"handle", QStringLiteral("Someone who's no longer your friend")}, {"avatarUrl", QString()},
                               {"close", false}, {"hidden", false}, {"muted", true}, {"isFriend", false}});
    }
    return out;
}

int StoriesSession::closeFriendsCount() const {
    std::vector<std::string> ids;
    for (const auto& [id, f] : friends_) ids.push_back(id);
    return static_cast<int>(st::recipients(ids, prefs_, true).size());
}

int StoriesSession::audienceCount() const {
    std::vector<std::string> ids;
    for (const auto& [id, f] : friends_) ids.push_back(id);
    return static_cast<int>(st::recipients(ids, prefs_, false).size());
}

void StoriesSession::setStage(const QString& stage) {
    if (postStage_ == stage) return;
    postStage_ = stage;
    emit composerChanged();
}

void StoriesSession::setMessages(const QString& error, const QString& toast) {
    error_ = error;
    toast_ = toast;
    emit messagesChanged();
}

void StoriesSession::clearMessages() {
    setMessages({}, {});
}

void StoriesSession::fail(const Failure& failure, const QString& fallback) {
    if (failure.status == 401) {
        emit sessionExpired();
        return;
    }
    setMessages(worded(failure, fallback), {});
}

// --- The feed ---

void StoriesSession::refresh() {
    auto api = api_;
    auto me = identity_;
    const auto chatKeys = chatKeysOf(chats_);
    const bool firstLoad = feedGeneration_ == 0;
    const int generation = ++feedGeneration_;
    const bool fillGaps = util::nowMillis() - lastGapFillMs_ > kGapFillEveryMs;
    if (fillGaps) lastGapFillMs_ = util::nowMillis();
    const auto prefs = prefs_;
    const auto seenHere = st::parseSeen(prefs::text(me->userId, kSeenKey).toStdString(), util::nowMillis());
    runInBackground(
        pool_, this,
        [api, me, chatKeys, firstLoad, generation, fillGaps, prefs, seenHere]() {
            FeedResult out;
            out.generation = generation;
            if (firstLoad) {
                out.config = sapi::config(*api);
                out.prefs = loadPrefs(*api, *me);
            }
            out.friends = loadFriends(*api, chatKeys);
            const auto feed = sapi::feed(*api);
            for (const auto& dto : feed.own) {
                if (auto d = decodeStory(dto, me->publicKey, me->privateKey)) {
                    d->seen = true;
                    out.own.push_back(std::move(d));
                }
            }
            std::sort(out.own.begin(), out.own.end(), [](const DecodedPtr& a, const DecodedPtr& b) { return a->createdMs < b->createdMs; });
            for (const auto& group : feed.authors) {
                const auto author = out.friends.find(group.authorId);
                if (author == out.friends.end()) continue;  // not a friend (any more)
                std::vector<DecodedPtr> stories;
                for (const auto& dto : group.stories) {
                    auto d = decodeStory(dto, author->second.publicKey, me->privateKey);
                    if (!d || d->dto.authorId != group.authorId) continue;
                    d->seen = d->seen || std::any_of(seenHere.begin(), seenHere.end(), [&](const st::SeenEntry& s) { return s.storyId == d->dto.id; });
                    stories.push_back(std::move(d));
                }
                std::sort(stories.begin(), stories.end(), [](const DecodedPtr& a, const DecodedPtr& b) { return a->createdMs < b->createdMs; });
                if (!stories.empty()) out.authors.emplace_back(group.authorId, std::move(stories));
            }
            // People with something new first (otherwise the server's order: most recent first).
            std::stable_sort(out.authors.begin(), out.authors.end(), [](const auto& a, const auto& b) {
                const auto unseen = [](const auto& g) { return std::any_of(g.second.begin(), g.second.end(), [](const DecodedPtr& d) { return !d->seen; }); };
                return unseen(a) && !unseen(b);
            });
            if (fillGaps) {
                try {
                    fillKeyGaps(*api, *me, out.own, out.friends, out.prefs ? out.prefs->prefs : prefs);
                } catch (const std::exception&) {
                    // Tried again on a later refresh.
                }
            }
            return out;
        },
        [this](FeedResult result) { applyFeed(result); },
        [this](const Failure& failure) {
            lastGapFillMs_ = 0;
            if (failure.status == 401) emit sessionExpired();
            // Offline: the tray keeps what it had.
        });
}

void StoriesSession::applyFeed(FeedResult& result) {
    if (result.generation != feedGeneration_) return;
    if (result.config) {
        mediaEnabled_ = result.config->media;
        interactiveEnabled_ = result.config->interactive;
        ttlChoices_ = result.config->ttlSeconds;
    }
    if (result.prefs) {
        prefs_ = result.prefs->prefs;
        archiveEnabled_ = result.prefs->archive;
        prefsRevision_ = result.prefs->revision;
    }
    friends_ = std::move(result.friends);
    own_ = std::move(result.own);
    authors_ = std::move(result.authors);
    for (const auto& d : own_) known_[d->dto.id] = d;
    for (const auto& [id, stories] : authors_) {
        for (const auto& d : stories) known_[d->dto.id] = d;
    }
    emit feedChanged();
    emit prefsChanged();
    syncOwnTotals();
}

// --- Viewing ---

void StoriesSession::openAuthor(const QString& authorIdText) {
    const std::string authorId = authorIdText.toStdString();
    viewerGroups_.clear();
    viewerStartGroup_ = 0;
    viewerStartStory_ = 0;
    viewerTitle_.clear();
    viewerHighlightId_.clear();
    const auto groupOf = [this](const std::string& owner, const std::vector<DecodedPtr>& stories) {
        QVariantList list;
        for (const auto& d : stories) list.append(storyMap(*d));
        return QVariantMap{{"owner", ownerMap(owner)}, {"stories", list}};
    };
    if (authorId == "me" || authorId == identity_->userId) {
        if (own_.empty()) return;
        viewerGroups_.append(groupOf(identity_->userId, own_));
    } else {
        const bool muted = has(prefs_.muted, authorId);
        for (const auto& [id, stories] : authors_) {
            if (muted ? id != authorId : has(prefs_.muted, id)) continue;
            if (id == authorId) {
                viewerStartGroup_ = static_cast<int>(viewerGroups_.size());
                const auto unseen = std::find_if(stories.begin(), stories.end(), [](const DecodedPtr& d) { return !d->seen; });
                viewerStartStory_ = unseen == stories.end() ? 0 : static_cast<int>(unseen - stories.begin());
            }
            viewerGroups_.append(groupOf(id, stories));
        }
        if (viewerGroups_.isEmpty()) return;
    }
    composerOpen_ = false;
    viewerOpen_ = true;
    emit composerChanged();
    emit viewerChanged();
}

void StoriesSession::openList(const std::string& ownerId, const std::vector<DecodedPtr>& stories, int index, const QString& title,
                              const QString& highlightId) {
    if (stories.empty()) return;
    QVariantList list;
    for (const auto& d : stories) {
        known_[d->dto.id] = d;
        list.append(storyMap(*d));
    }
    viewerGroups_ = {QVariantMap{{"owner", ownerMap(ownerId)}, {"stories", list}}};
    viewerStartGroup_ = 0;
    viewerStartStory_ = std::clamp(index, 0, static_cast<int>(stories.size()) - 1);
    viewerTitle_ = title;
    viewerHighlightId_ = highlightId;
    composerOpen_ = false;
    viewerOpen_ = true;
    emit composerChanged();
    emit viewerChanged();
}

void StoriesSession::openHighlight(const QString& ownerId, const QString& highlightId) {
    const auto it = highlights_.find(highlightId.toStdString());
    if (it == highlights_.end()) return;
    const bool mine = ownerId.toStdString() == identity_->userId;
    openList(it->second.ownerId, it->second.stories, 0, qs(it->second.title.empty() ? "Highlight" : it->second.title), mine ? highlightId : QString());
}

void StoriesSession::openLibrary(int index) {
    openList(identity_->userId, libraryStories_, index, QStringLiteral("Archive"), {});
}

void StoriesSession::closeViewer() {
    if (!viewerOpen_) return;
    viewerOpen_ = false;
    viewerGroups_.clear();
    emit viewerChanged();
    dropTempFiles();
}

void StoriesSession::dropTempFiles() {
    for (const auto& file : tempFiles_) QFile::remove(file);
    tempFiles_.clear();
}

void StoriesSession::loadMedia(const QString& storyIdText) {
    const std::string storyId = storyIdText.toStdString();
    const auto d = find(storyId);
    if (!d || !d->meta.media) {
        emit mediaFailed(storyIdText);
        return;
    }
    if (d->meta.kind == "image" && ChatImageCache::instance().has(imageKey(storyId))) {
        emit mediaReady(storyIdText, QStringLiteral("image://chatimage/full/") + imageKey(storyId));
        return;
    }
    auto api = api_;
    const auto media = *d->meta.media;
    const auto key = crypto::base64Decode(media.key);
    const auto header = crypto::base64Decode(media.header);
    const std::string kind = d->meta.kind;
    runInBackground(
        pool_, this,
        [api, storyId, media, key, header, kind]() -> QString {
            std::istringstream in(sapi::downloadMedia(*api, storyId));
            std::ostringstream out;
            crypto::files::decrypt(in, out, key, header, media.encryptedSize, media.sha256);
            const std::string plain = out.str();
            if (kind == "image") {
                const QByteArray bytes(plain.data(), static_cast<qsizetype>(plain.size()));
                const auto image = ChatImageCache::decode(bytes);
                if (!image) throw std::runtime_error("Not an image");
                ChatImageCache::instance().put(imageKey(storyId), ChatImageCache::thumbnail(*image), bytes);
                return QStringLiteral("image://chatimage/full/") + imageKey(storyId);
            }
            // The player needs a file: a private one, deleted when the viewer closes.
            const QString dir = mediaDir();
            QDir().mkpath(dir);
            const QString ext = media.mime == "video/webm" ? "webm" : media.mime == "video/quicktime" ? "mov" : "mp4";
            const QString path = dir + u'/' + QString::fromStdString(util::randomUuid()) + u'.' + ext;
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(plain.data(), static_cast<qint64>(plain.size())) != static_cast<qint64>(plain.size())) {
                QFile::remove(path);
                throw std::runtime_error("Couldn't write the video");
            }
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            return path;
        },
        [this, storyIdText, kind](QString location) {
            if (kind == "image") {
                emit mediaReady(storyIdText, location);
                return;
            }
            tempFiles_.append(location);
            emit mediaReady(storyIdText, QUrl::fromLocalFile(location).toString());
        },
        [this, storyIdText](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            emit mediaFailed(storyIdText);
        });
}

void StoriesSession::markSeen(const QString& storyIdText) {
    const std::string storyId = storyIdText.toStdString();
    const auto d = find(storyId);
    if (!d || d->seen || d->dto.authorId == identity_->userId) return;
    d->seen = true;
    auto seen = st::parseSeen(prefs::text(identity_->userId, kSeenKey).toStdString(), util::nowMillis());
    seen.push_back({storyId, util::nowMillis()});
    prefs::setText(identity_->userId, kSeenKey, qs(st::toJson(seen)));
    if (prefs_.shareViews) {
        auto api = api_;
        runInBackground(pool_, this, [api, storyId] { sapi::markViewed(*api, storyId); }, [] {}, [](const Failure&) {});
    }
    emit feedChanged();
}

bool StoriesSession::canReply(const QString& storyId) const {
    const auto d = find(storyId.toStdString());
    if (!d || !d->dto.allowReplies || d->dto.authorId == identity_->userId || !chats_) return false;
    return !chats_->chatIdForUser(qs(d->dto.authorId)).isEmpty();
}

void StoriesSession::reply(const QString& storyId, const QString& text, bool reaction) {
    const auto d = find(storyId.toStdString());
    if (!d || !chats_ || text.trimmed().isEmpty()) return;
    auto meta = chats_->metaFor(chats_->chatIdForUser(qs(d->dto.authorId)).toStdString());
    if (!meta) {
        setMessages(QStringLiteral("You can reply once your chat with them has loaded."), {});
        return;
    }
    if (const QString why = chats_->sendRefusal(meta->id, device_key_refusal::kStoryReply); !why.isEmpty()) {
        setMessages(why, {});
        return;
    }
    auto repository = chats_->repository();
    const model::StoryReplyRef ref{d->dto.authorId, d->dto.id, d->meta.kind, st::preview(d->meta), reaction};
    const std::string body = text.trimmed().toStdString();
    runInBackground(
        pool_, this, [repository, meta, body, ref] { repository->sendStoryReply(*meta, body, ref); },
        [this, reaction, text] {
            setMessages({}, reaction ? QStringLiteral("Sent ") + text : QStringLiteral("Reply sent"));
            if (chats_) chats_->refreshNow();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't send. Try again.")); });
}

void StoriesSession::deleteStory(const QString& storyIdText) {
    const std::string storyId = storyIdText.toStdString();
    auto api = api_;
    runInBackground(
        pool_, this, [api, storyId] { sapi::remove(*api, storyId); },
        [this, storyId] {
            own_.erase(std::remove_if(own_.begin(), own_.end(), [&](const DecodedPtr& d) { return d->dto.id == storyId; }), own_.end());
            known_.erase(storyId);
            emit feedChanged();
            setMessages({}, QStringLiteral("Story deleted"));
            refresh();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't delete. Try again.")); });
}

void StoriesSession::loadViewers(const QString& storyIdText) {
    const std::string storyId = storyIdText.toStdString();
    auto api = api_;
    runInBackground(
        pool_, this, [api, storyId] { return sapi::viewers(*api, storyId); },
        [this, storyIdText](std::vector<std::string> ids) {
            QVariantList out;
            for (const auto& id : ids) {
                const auto f = friends_.find(id);
                out.append(QVariantMap{{"userId", qs(id)},
                                       {"handle", f == friends_.end() ? QStringLiteral("Someone who's no longer your friend") : qs(f->second.handle)},
                                       {"avatarUrl", f == friends_.end() ? QString() : qs(f->second.avatarUrl.value_or(""))}});
            }
            emit viewersLoaded(storyIdText, out);
        },
        [this, storyIdText](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            emit viewersLoaded(storyIdText, {});
        });
}

bool StoriesSession::isMuted(const QString& userId) const {
    return has(prefs_.muted, userId.toStdString());
}

void StoriesSession::setMuted(const QString& userIdText, bool muted) {
    const std::string userId = userIdText.toStdString();
    savePrefs(
        [userId, muted](st::Prefs p) {
            p.muted.erase(std::remove(p.muted.begin(), p.muted.end(), userId), p.muted.end());
            if (muted) p.muted.push_back(userId);
            return p;
        },
        std::nullopt);
}

// --- Settings ---

void StoriesSession::savePrefs(std::function<st::Prefs(st::Prefs)> change, std::optional<bool> archive) {
    auto api = api_;
    auto me = identity_;
    const PrefsResult start{prefs_, archiveEnabled_, prefsRevision_};
    runInBackground(
        pool_, this,
        [api, me, change, archive, start]() {
            PrefsResult base = start;
            for (int attempt = 0; attempt < kMaxPrefsAttempts; ++attempt) {
                const st::Prefs next = change(base.prefs);
                const auto sealed = sc::sealPrefs(st::toJson(next), me->privateKey);
                try {
                    const auto saved = sapi::putPrefs(*api, crypto::base64Encode(sealed.ciphertext), crypto::base64Encode(sealed.nonce),
                                                      archive.value_or(base.archive), base.revision);
                    return std::make_pair(PrefsResult{next, saved.archiveEnabled, saved.revision}, base.prefs);
                } catch (const api::ApiError& e) {
                    if (e.status() != 409) throw;
                    base = loadPrefs(*api, *me);  // another device saved first: apply the change to theirs
                }
            }
            throw api::ApiError(409, "prefs_conflict", "Couldn't save your story settings. Try again.");
        },
        [this](std::pair<PrefsResult, st::Prefs> saved) {
            const st::Prefs before = saved.second;
            prefs_ = saved.first.prefs;
            archiveEnabled_ = saved.first.archive;
            prefsRevision_ = saved.first.revision;
            emit prefsChanged();
            emit feedChanged();

            // Who can see what you've already posted follows the new settings.
            std::vector<std::string> nowHidden, leftClose, joinedClose;
            for (const auto& u : prefs_.hideFrom) {
                if (!has(before.hideFrom, u)) nowHidden.push_back(u);
            }
            for (const auto& u : before.closeFriends) {
                if (!has(prefs_.closeFriends, u)) leftClose.push_back(u);
            }
            for (const auto& u : prefs_.closeFriends) {
                if (!has(before.closeFriends, u) && !has(prefs_.hideFrom, u)) joinedClose.push_back(u);
            }
            std::vector<DecodedPtr> closeStories;
            for (const auto& d : own_) {
                if (d->meta.closeFriends) closeStories.push_back(d);
            }
            std::map<std::string, crypto::Bytes> joinedKeys;
            for (const auto& u : joinedClose) {
                if (const auto f = friends_.find(u); f != friends_.end()) joinedKeys[u] = f->second.publicKey;
            }
            if (std::any_of(before.hideFrom.begin(), before.hideFrom.end(), [&](const std::string& u) { return !has(prefs_.hideFrom, u); })) {
                lastGapFillMs_ = 0;  // un-hidden friends get keys for "friends" stories on the next top-up
            }
            if (nowHidden.empty() && (closeStories.empty() || (leftClose.empty() && joinedKeys.empty()))) {
                refresh();
                return;
            }
            auto api = api_;
            auto me = identity_;
            runInBackground(
                pool_, this,
                [api, me, nowHidden, leftClose, joinedKeys, closeStories] {
                    for (const auto& u : nowHidden) sapi::revoke(*api, u);
                    std::vector<std::string> closeIds;
                    for (const auto& d : closeStories) closeIds.push_back(d->dto.id);
                    if (!closeIds.empty()) {
                        for (const auto& u : leftClose) sapi::revoke(*api, u, closeIds);
                        for (const auto& [u, publicKey] : joinedKeys) {
                            for (const auto& d : closeStories) sapi::addKeys(*api, d->dto.id, {{u, wrapFor(d->dto.id, *d->key, publicKey, *me)}});
                        }
                    }
                },
                [this] { refresh(); }, [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't update who sees your stories.")); });
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't save. Try again.")); });
}

void StoriesSession::setCloseFriends(const QStringList& userIds) {
    const auto ids = stdList(userIds);
    savePrefs([ids](st::Prefs p) { p.closeFriends = ids; return p; }, std::nullopt);
}

void StoriesSession::setHideFrom(const QStringList& userIds) {
    const auto ids = stdList(userIds);
    savePrefs([ids](st::Prefs p) { p.hideFrom = ids; return p; }, std::nullopt);
}

void StoriesSession::setShareViews(bool on) {
    savePrefs([on](st::Prefs p) { p.shareViews = on; return p; }, std::nullopt);
}

void StoriesSession::setAllowReplies(bool on) {
    savePrefs([on](st::Prefs p) { p.allowReplies = on; return p; }, std::nullopt);
}

void StoriesSession::setArchive(bool on) {
    savePrefs([](st::Prefs p) { return p; }, on);
}

// --- Posting ---

void StoriesSession::openComposer() {
    closeViewer();
    clearMessages();
    composerOpen_ = true;
    emit composerChanged();
    refreshDrafts();
}

void StoriesSession::closeComposer() {
    if (!postStage_.isEmpty()) return;
    composerOpen_ = false;
    emit composerChanged();
    dropDraftFiles();
}

QVariantMap StoriesSession::checkFile(const QUrl& file) const {
    const QFileInfo info(file.toLocalFile());
    const QString ext = info.suffix().toLower();
    if (QStringList{"jpg", "jpeg", "png", "webp", "gif", "bmp"}.contains(ext)) return {{"kind", "image"}, {"error", QString()}};
    if (QStringList{"mp4", "webm", "mov"}.contains(ext)) {
        if (!mediaEnabled_) return {{"kind", "video"}, {"error", QStringLiteral("Photo and video stories aren't available on this server yet.")}};
        if (static_cast<std::uint64_t>(info.size()) > st::kMaxVideoBytes) {
            return {{"kind", "video"}, {"error", QStringLiteral("That video is too large for a story (at most 60 MB).")}};
        }
        return {{"kind", "video"}, {"error", QString()}};
    }
    return {{"kind", QString()}, {"error", QStringLiteral("Use a photo (JPEG, PNG, WebP) or a video (MP4, WebM, MOV).")}};
}

std::optional<std::vector<st::Sticker>> StoriesSession::stickersFrom(const QVariantList& list) {
    std::vector<st::Sticker> candidates;
    for (const auto& v : list) candidates.push_back(stickerFromMap(v.toMap()));
    // The same parser every reader uses: what it drops, a reader would drop too.
    auto checked = st::parseStickers(st::toJson(candidates));
    if (checked.size() != candidates.size()) {
        setMessages(QStringLiteral("Some stickers couldn't be used. Check each one and try again."), {});
        return std::nullopt;
    }
    if (!interactiveEnabled_ && std::any_of(checked.begin(), checked.end(), [](const st::Sticker& s) { return st::isAnswerable(s); })) {
        setMessages(QStringLiteral("Polls, quizzes, questions and sliders aren't available on this server."), {});
        return std::nullopt;
    }
    return checked;
}

int StoriesSession::recipientCount(const QString& audience, const QStringList& customIds) const {
    std::vector<std::string> ids;
    for (const auto& [id, f] : friends_) ids.push_back(id);
    return static_cast<int>(st::recipients(ids, prefs_, audienceOf(audience), stdList(customIds)).size());
}

void StoriesSession::postText(const QString& text, const QString& background, const QString& font, const QVariantList& overlays,
                              const QVariantList& stickers, const QString& audienceText, const QStringList& customIds, bool allowReplies,
                              int ttlSeconds) {
    if (!postStage_.isEmpty()) return;
    const auto parsedStickers = stickersFrom(stickers);
    if (!parsedStickers) return;
    const std::string audience = audienceOf(audienceText);
    st::Meta meta;
    meta.kind = "text";
    meta.text = text.trimmed().left(static_cast<int>(st::kMaxStoryText)).toStdString();
    meta.background = background.toStdString();
    meta.font = font.toStdString();
    for (const auto& o : overlays) meta.overlays.push_back(overlayFrom(o.toMap()));
    meta.stickers = *parsedStickers;
    meta.closeFriends = audience == "close_friends";
    try {
        meta = st::parseMeta(st::toJson(meta), "text");  // the same checks every reader makes
    } catch (const st::StoryFormatError&) {
        setMessages(QStringLiteral("Write something first."), {});
        return;
    }
    publish(std::move(meta), {}, audience, stdList(customIds), allowReplies, ttlSeconds);
}

void StoriesSession::postMedia(const QUrl& file, const QVariantList& overlays, const QVariantList& stickers, const QString& audienceText,
                               const QStringList& customIds, bool allowReplies, int ttlSeconds, int durationMs) {
    if (!postStage_.isEmpty()) return;
    const auto parsedStickers = stickersFrom(stickers);
    if (!parsedStickers) return;
    const std::string audience = audienceOf(audienceText);
    const bool closeFriends = audience == "close_friends";
    const auto customList = stdList(customIds);
    const QVariantMap check = checkFile(file);
    if (!check.value("error").toString().isEmpty()) {
        setMessages(check.value("error").toString(), {});
        return;
    }
    if (!mediaEnabled_) {
        setMessages(QStringLiteral("Photo and video stories aren't available on this server yet."), {});
        return;
    }
    std::vector<st::Overlay> parsedOverlays;
    for (const auto& o : overlays) parsedOverlays.push_back(overlayFrom(o.toMap()));
    const QString path = file.toLocalFile();
    const bool video = check.value("kind").toString() == "video";
    if (video && (durationMs <= 0 || durationMs > st::kMaxVideoMs + 500)) {
        setMessages(durationMs <= 0 ? QStringLiteral("That video couldn't be opened.")
                                    : QStringLiteral("Stories can be up to 60 seconds long. Trim the video first."),
                    {});
        return;
    }
    clearMessages();
    setStage(QStringLiteral("preparing"));
    runInBackground(
        pool_, this,
        [path, video, durationMs, parsedOverlays, stickerList = *parsedStickers, closeFriends]() {
            st::Meta meta;
            meta.overlays = parsedOverlays;
            meta.stickers = stickerList;
            meta.closeFriends = closeFriends;
            PreparedMedia prepared = prepareMedia(path, video, durationMs);
            meta.kind = prepared.kind;
            meta.media = prepared.media;
            return std::make_pair(meta, std::move(prepared.bytes));
        },
        [this, audience, customList, allowReplies, ttlSeconds](std::pair<st::Meta, std::string> prepared) {
            setStage({});
            publish(std::move(prepared.first), std::move(prepared.second), audience, customList, allowReplies, ttlSeconds);
        },
        [this](const Failure& failure) {
            setStage({});
            setMessages(failure.message.isEmpty() ? QStringLiteral("That file couldn't be used.") : failure.message, {});
        });
}

void StoriesSession::publish(st::Meta meta, std::string media, const std::string& audience, const std::vector<std::string>& customIds,
                             bool allowReplies, int ttlSeconds) {
    auto api = api_;
    auto me = identity_;
    const auto chatKeys = chatKeysOf(chats_);
    const auto prefs = prefs_;
    // Only a lifetime the server offers is sent (an older server knows only 24 hours).
    const bool sendTtl = ttlSeconds > 0 && std::find(ttlChoices_.begin(), ttlChoices_.end(), ttlSeconds) != ttlChoices_.end();
    setStage(QStringLiteral("encrypting"));
    runInBackground(
        pool_, this,
        [api, me, chatKeys, prefs, meta, media = std::move(media), audience, customIds, allowReplies, ttlSeconds, sendTtl]() mutable {
            const auto friends = loadFriends(*api, chatKeys);
            std::vector<std::string> friendIds;
            for (const auto& [id, f] : friends) friendIds.push_back(id);
            const auto recipients = st::recipients(friendIds, prefs, audience, customIds);
            if (recipients.empty() && audience != "friends") {
                throw api::UserFacingError(audience == "custom" ? "Choose at least one friend." : "You have no close friends yet.");
            }

            const std::string id = util::randomUuid();
            const crypto::SecretBytes key = sc::newStoryKey();
            std::string ciphertext;
            if (meta.media) {
                std::istringstream in(media);
                std::ostringstream out;
                const auto encrypted = crypto::files::encrypt(in, out);
                ciphertext = out.str();
                meta.media->key = crypto::base64Encode(encrypted.key);
                meta.media->header = crypto::base64Encode(encrypted.header);
                meta.media->sha256 = encrypted.ciphertextSha256;
                meta.media->encryptedSize = encrypted.encryptedSize;
                meta.media->chunkSize = crypto::files::kChunkBytes;
                // Best effort: the plaintext doesn't linger in this copy.
                std::fill(media.begin(), media.end(), '\0');
            }
            const auto sealed = sc::sealMeta(id, key, st::toJson(meta));
            sapi::NewStory story;
            story.id = id;
            story.kind = meta.kind;
            story.metaCiphertext = crypto::base64Encode(sealed.ciphertext);
            story.metaNonce = crypto::base64Encode(sealed.nonce);
            if (meta.media) story.mediaSize = meta.media->encryptedSize;
            // "friends" stories reach friends added later too (key top-ups); close-friends and chosen-people ones don't.
            story.audience = audience;
            story.allowReplies = allowReplies;
            if (sendTtl) story.ttlSeconds = ttlSeconds;
            story.keys.push_back({me->userId, wrapFor(id, key, me->publicKey, *me)});
            for (const auto& r : recipients) story.keys.push_back({r, wrapFor(id, key, friends.at(r).publicKey, *me)});
            sapi::create(*api, story);
            if (meta.media) {
                try {
                    sapi::uploadMedia(*api, id, std::move(ciphertext));
                } catch (...) {
                    try {
                        sapi::remove(*api, id);
                    } catch (...) {
                    }
                    throw;
                }
            }
        },
        [this] {
            setStage({});
            composerOpen_ = false;
            emit composerChanged();
            dropDraftFiles();
            setMessages({}, QStringLiteral("Shared to your story"));
            emit posted();
            refresh();
        },
        [this](const Failure& failure) {
            setStage({});
            fail(failure, QStringLiteral("Couldn't share your story. Try again."));
        });
}

// --- Stickers ---

namespace {

/// The author's app counts their own stories' totals this often in the background (opening the story does it at once).
constexpr std::int64_t kTotalsEveryMs = 3 * 60 * 1000;

/**
 * Opens one story's stickers (blocking). A viewer's side: what they already answered and the totals the author published.
 * The author's side: every answer sealed to them, counted, and the totals friends may see published (only what changed).
 */
stories_detail::InteractionsResult openInteractions(api::ApiClient& api, const data::SelfIdentity& me, const std::string& storyId,
                                                    const crypto::SecretBytes& storyKey, const std::vector<st::Sticker>& stickers, bool own,
                                                    const crypto::Bytes& authorKey, const std::map<std::string, crypto::Bytes>& friendKeys,
                                                    std::map<std::string, std::string> published) {
    using stories_detail::MyAnswer;
    stories_detail::InteractionsResult out;
    out.storyId = storyId;
    out.own = own;
    const auto stickerById = [&](const std::string& id) -> const st::Sticker* {
        for (const auto& s : stickers) {
            if (s.id == id && st::isAnswerable(s)) return &s;
        }
        return nullptr;
    };

    if (!own) {
        for (const auto& row : sapi::myAnswers(api, storyId)) {
            const st::Sticker* sticker = stickerById(row.stickerId);
            if (!sticker) continue;
            MyAnswer mine;
            mine.anonymous = row.anonymous;
            mine.answer.kind = sticker->type;
            if (!row.anonymous) {
                try {
                    mine.answer = sc::openAnswer(storyId, *sticker, crypto::base64Decode(row.envelope), authorKey, me.privateKey);
                    mine.readable = true;
                } catch (const std::exception&) {
                    // Answered, but it can't be read back here.
                }
            }
            out.mine[row.stickerId] = mine;
        }
        for (const auto& row : sapi::totals(api, storyId)) {
            const st::Sticker* sticker = stickerById(row.stickerId);
            if (!sticker || sticker->type == "question") continue;
            try {
                out.totals[row.stickerId] = sc::openTotals(storyId, *sticker, static_cast<std::uint32_t>(std::max(row.revision, 0)),
                                                           crypto::base64Decode(row.ciphertext), crypto::base64Decode(row.nonce), storyKey);
            } catch (const std::exception&) {
                // Forged, for another sticker, or malformed: not shown.
            }
        }
        return out;
    }

    for (const auto& row : sapi::answers(api, storyId)) {
        const st::Sticker* sticker = stickerById(row.stickerId);
        if (!sticker) {
            ++out.unreadable;
            continue;
        }
        try {
            const crypto::Bytes envelope = crypto::base64Decode(row.envelope);
            st::Answer answer;
            if (row.anonymous || row.responderId.empty()) {
                answer = sc::openAnonymousAnswer(storyId, *sticker, envelope, me.publicKey, me.privateKey);
            } else {
                const auto responder = friendKeys.find(row.responderId);
                if (responder == friendKeys.end()) throw std::runtime_error("not a friend");
                answer = sc::openAnswer(storyId, *sticker, envelope, responder->second, me.privateKey);
            }
            st::InboxEntry entry;
            entry.stickerId = row.stickerId;
            entry.responderId = row.anonymous ? std::string() : row.responderId;
            entry.answer = std::move(answer);
            entry.atMs = util::parseIsoMillis(row.respondedAt).value_or(0);
            entry.revision = row.revision;
            entry.envelope = row.envelope;
            out.entries.push_back(std::move(entry));
        } catch (const std::exception&) {
            ++out.unreadable;
        }
    }
    for (const auto& sticker : stickers) {
        if (st::isAnswerable(sticker)) out.totals[sticker.id] = st::tally(sticker, out.entries);
    }
    std::map<std::string, int> current;
    for (const auto& row : sapi::totals(api, storyId)) current[row.stickerId] = row.revision;
    for (const auto& sticker : stickers) {
        if (!st::isAnswerable(sticker) || sticker.type == "question" || !sticker.resultsEveryone) continue;
        const st::Totals& totals = out.totals[sticker.id];
        const std::string fingerprint = st::toJson(totals);
        const std::string cacheKey = storyId + ":" + sticker.id;
        const auto stored = current.find(sticker.id);
        if (published[cacheKey] == fingerprint && stored != current.end()) continue;
        const int revision = (stored == current.end() ? 0 : stored->second) + 1;
        const auto sealed = sc::sealTotals(storyId, sticker.id, static_cast<std::uint32_t>(revision), totals, storyKey);
        try {
            sapi::putTotals(api, storyId, sticker.id, revision, crypto::base64Encode(sealed.nonce), crypto::base64Encode(sealed.ciphertext));
            published[cacheKey] = fingerprint;
        } catch (const api::ApiError& e) {
            // A second device publishing at the same moment just loses the race; the next round catches up.
            if (e.status() != 409) throw;
        }
    }
    out.published = std::move(published);
    return out;
}

}  // namespace

QVariantList StoriesSession::lifetimes() const {
    std::vector<int> choices = ttlChoices_;
    if (choices.empty()) choices.push_back(st::kDefaultStorySeconds);  // a server that predates this: 24 hours only
    QVariantList out;
    for (const int seconds : choices) out.append(QVariantMap{{"seconds", seconds}, {"label", qs(st::lifetimeLabel(seconds))}});
    return out;
}

QVariantMap StoriesSession::checkSticker(const QVariantMap& candidate) const {
    const auto refuse = [](const QString& error) { return QVariantMap{{"ok", false}, {"error", error}, {"sticker", QVariantMap()}}; };
    st::Sticker s = stickerFromMap(candidate);
    if (s.id.empty()) s.id = newStickerId();
    if (s.type == "countdown" && s.when <= static_cast<double>(util::nowMillis())) return refuse(QStringLiteral("Pick a time in the future."));
    if (s.type == "link") {
        const auto link = st::safeStoryLink(s.url);
        if (!link) return refuse(QStringLiteral("Use a full https:// address (no sign-in details in it)."));
        s.url = *link;
        if (qs(s.text).trimmed().isEmpty()) s.text = hostOf(*link);
    }
    if (s.type == "mention" && !friends_.count(s.userId)) return refuse(QStringLiteral("Choose a friend."));
    // The parser every reader uses decides: what it drops, a reader would drop too.
    const auto parsed = st::parseStickers("[" + st::toJson(s) + "]");
    if (parsed.empty()) return refuse(QStringLiteral("Fill in every field. Options can be up to 40 characters; questions up to 120."));
    return {{"ok", true}, {"error", QString()}, {"sticker", stickerMap(parsed.front())}};
}

void StoriesSession::loadInteractions(const QString& storyIdText) {
    if (const auto d = find(storyIdText.toStdString())) startInteractions(d);
}

void StoriesSession::startInteractions(const std::shared_ptr<Decoded>& d) {
    if (!interactiveEnabled_) return;
    const bool answerable = std::any_of(d->meta.stickers.begin(), d->meta.stickers.end(), [](const st::Sticker& s) { return st::isAnswerable(s); });
    if (!answerable) return;
    const bool own = d->dto.authorId == identity_->userId;
    crypto::Bytes authorKey = identity_->publicKey;
    if (!own) {
        const auto author = friends_.find(d->dto.authorId);
        if (author == friends_.end()) return;  // not a friend (any more): nothing to open it with
        authorKey = author->second.publicKey;
    }
    std::map<std::string, crypto::Bytes> friendKeys;
    for (const auto& [id, f] : friends_) friendKeys[id] = f.publicKey;
    auto api = api_;
    auto me = identity_;
    const std::string storyId = d->dto.id;
    const auto key = d->key;
    const auto stickers = d->meta.stickers;
    const auto published = publishedTotals_;
    runInBackground(
        pool_, this,
        [api, me, storyId, key, stickers, own, authorKey, friendKeys, published]() {
            return openInteractions(*api, *me, storyId, *key, stickers, own, authorKey, friendKeys, published);
        },
        [this](InteractionsResult r) {
            for (const auto& [cacheKey, fingerprint] : r.published) publishedTotals_[cacheKey] = fingerprint;
            QVariantMap mine;
            for (const auto& [stickerId, a] : r.mine) mine[qs(stickerId)] = mineMap(a);
            QVariantMap totals;
            for (const auto& [stickerId, t] : r.totals) totals[qs(stickerId)] = totalsMap(t);
            QVariantList entries;
            for (const auto& e : r.entries) {
                if (e.answer.kind != "question") continue;
                QString sender = QStringLiteral("Anonymous");
                if (!e.responderId.empty()) {
                    const auto f = friends_.find(e.responderId);
                    sender = f == friends_.end() ? QStringLiteral("Someone who's no longer your friend") : qs(f->second.handle);
                }
                entries.append(QVariantMap{{"stickerId", qs(e.stickerId)},
                                           {"sender", sender},
                                           {"text", qs(e.answer.text)},
                                           {"atMs", static_cast<double>(e.atMs)},
                                           {"anonymous", e.responderId.empty()},
                                           {"envelope", qs(e.envelope)}});
            }
            emit interactionsLoaded(qs(r.storyId), QVariantMap{{"own", r.own},
                                                               {"mine", mine},
                                                               {"totals", totals},
                                                               {"entries", entries},
                                                               {"answerCount", static_cast<int>(r.entries.size())},
                                                               {"unreadable", r.unreadable}});
        },
        [this, storyId](const Failure& failure) {
            lastTotalsMs_.erase(storyId);
            if (failure.status == 401) emit sessionExpired();
            // Otherwise offline or refused: what the viewer already shows stays.
        });
}

void StoriesSession::syncOwnTotals() {
    if (!interactiveEnabled_) return;
    const std::int64_t now = util::nowMillis();
    for (const auto& d : own_) {
        const bool shows = std::any_of(d->meta.stickers.begin(), d->meta.stickers.end(),
                                       [](const st::Sticker& s) { return st::isAnswerable(s) && s.type != "question" && s.resultsEveryone; });
        if (!shows) continue;
        std::int64_t& last = lastTotalsMs_[d->dto.id];
        if (now - last < kTotalsEveryMs) continue;
        last = now;
        startInteractions(d);
    }
}

void StoriesSession::answer(const QString& storyIdText, const QString& stickerIdText, const QVariantMap& given) {
    const std::string storyId = storyIdText.toStdString();
    const std::string stickerId = stickerIdText.toStdString();
    const auto d = find(storyId);
    if (!d || !interactiveEnabled_ || d->dto.authorId == identity_->userId) return;
    const auto found = std::find_if(d->meta.stickers.begin(), d->meta.stickers.end(),
                                    [&](const st::Sticker& s) { return s.id == stickerId && st::isAnswerable(s); });
    if (found == d->meta.stickers.end()) return;
    const st::Sticker sticker = *found;
    const auto author = friends_.find(d->dto.authorId);
    if (author == friends_.end()) {
        setMessages(QStringLiteral("Can't answer right now."), {});
        return;
    }
    const crypto::Bytes authorKey = author->second.publicKey;
    st::Answer answer;
    answer.kind = sticker.type;
    answer.choice = given.value("choice").toInt();
    answer.value = given.value("value").toInt();
    answer.text = given.value("text").toString().toStdString();
    st::Answer checked;
    try {
        checked = st::checkedAnswer(answer, sticker);
    } catch (const st::StoryFormatError&) {
        setMessages(QStringLiteral("That answer can't be sent."), {});
        return;
    }
    const bool anonymous = st::isAnonymousSticker(sticker);
    const bool final = st::isFinalSticker(sticker);
    auto api = api_;
    auto me = identity_;
    runInBackground(
        pool_, this,
        [api, me, storyId, stickerId, sticker, checked, authorKey, anonymous, final]() {
            const crypto::Bytes envelope = anonymous ? sc::sealAnonymousAnswer(storyId, sticker, checked, authorKey)
                                                     : sc::sealAnswer(storyId, sticker, checked, authorKey, me->privateKey);
            sapi::putAnswer(*api, storyId, stickerId, crypto::base64Encode(envelope), anonymous, final);
        },
        [this, storyIdText, stickerIdText, sticker, checked, anonymous] {
            stories_detail::MyAnswer mine;
            mine.anonymous = anonymous;
            mine.readable = !anonymous;
            mine.answer.kind = sticker.type;
            if (!anonymous) mine.answer = checked;
            setMessages({}, sticker.type == "question" ? QStringLiteral("Sent") : QStringLiteral("Answer sent"));
            emit answered(storyIdText, stickerIdText, mineMap(mine));
            loadInteractions(storyIdText);  // the totals, now that you may see them
        },
        [this, storyIdText](const Failure& failure) {
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            if (failure.status == 409) {
                setMessages(QStringLiteral("You already answered that."), {});
                loadInteractions(storyIdText);
            } else if (failure.status == 404) {
                setMessages(QStringLiteral("This story has ended."), {});
            } else {
                setMessages(QStringLiteral("Couldn't send. Try again."), {});
            }
        });
}

QString StoriesSession::storyEvidence(const QString& storyId) const {
    const auto d = find(storyId.toStdString());
    return d ? evidenceOf(d->meta) : QString();
}

void StoriesSession::reportStory(const QString& storyIdText, const QString& reason, const QString& details, bool includeWords) {
    const std::string storyId = storyIdText.toStdString();
    const auto d = find(storyId);
    if (!d) return;
    QString why = reason.trimmed();
    if (!details.trimmed().isEmpty()) why += QStringLiteral(": ") + details.trimmed();
    why = why.left(300);
    std::optional<std::string> evidence;
    if (includeWords) {
        if (const QString words = evidenceOf(d->meta); !words.isEmpty()) evidence = words.toStdString();
    }
    auto api = api_;
    const std::string reasonText = why.toStdString();
    runInBackground(
        pool_, this, [api, storyId, reasonText, evidence] { sapi::reportStory(*api, storyId, reasonText, evidence); },
        [this] {
            setMessages({}, QStringLiteral("Reported. Thank you."));
            emit reportDone(true, QStringLiteral("Reported. Thank you."));
        },
        [this](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            emit reportDone(false, failure.status == 409 ? QStringLiteral("You already reported this story.") : QStringLiteral("Couldn't send the report. Try again."));
        });
}

void StoriesSession::reportAnswer(const QString& storyIdText, const QString& stickerIdText, const QString& envelope, const QString& reason,
                                  const QString& answerText, bool includeText) {
    const std::string storyId = storyIdText.toStdString();
    const std::string stickerId = stickerIdText.toStdString();
    const std::string envelopeText = envelope.toStdString();
    const std::string why = (reason.trimmed().isEmpty() ? QStringLiteral("Abusive answer") : reason.trimmed().left(300)).toStdString();
    std::optional<std::string> evidence;
    if (includeText && !answerText.isEmpty()) evidence = answerText.left(600).toStdString();
    auto api = api_;
    runInBackground(
        pool_, this, [api, storyId, stickerId, envelopeText, why, evidence] { sapi::reportAnswer(*api, storyId, stickerId, envelopeText, why, evidence); },
        [this] {
            setMessages({}, QStringLiteral("Reported. RedVault's admins can see who sent it."));
            emit reportDone(true, QStringLiteral("Reported. RedVault's admins can see who sent it."));
        },
        [this](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            emit reportDone(false, failure.status == 409 ? QStringLiteral("You already reported this answer.")
                                                          : worded(failure, QStringLiteral("Couldn't send the report.")));
        });
}

// --- Drafts ---

namespace {

struct LoadedDraftResult {
    data::LoadedDraft draft;
    QString mediaFile;
};

}  // namespace

void StoriesSession::dropDraftFiles() {
    for (const auto& file : draftFiles_) QFile::remove(file);
    draftFiles_.clear();
}

void StoriesSession::refreshDrafts() {
    auto me = identity_;
    const data::StoryDraftStore* store = draftStore_.get();
    runInBackground(
        pool_, this, [store, me] { return store->list(me->userId, me->privateKey); },
        [this](std::vector<model::stories::DraftSummary> list) {
            QVariantList out;
            for (const auto& d : list) {
                out.append(QVariantMap{{"id", qs(d.id)},
                                       {"preview", qs(d.preview)},
                                       {"kind", qs(d.kind)},
                                       {"stickerCount", d.stickerCount},
                                       {"updatedAt", static_cast<double>(d.updatedAtMs)}});
            }
            drafts_ = out;
            emit draftsChanged();
        },
        [this](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
        });
}

void StoriesSession::saveDraft(const QString& draftIdText, const QVariantMap& state, int durationMs) {
    if (!postStage_.isEmpty()) return;
    model::stories::DraftContent content;
    content.mode = state.value("mode").toString() == QStringLiteral("media") ? "media" : "text";
    content.text = state.value("text").toString().toStdString();
    content.background = state.value("background", QStringLiteral("sunset")).toString().toStdString();
    content.font = state.value("font", QStringLiteral("classic")).toString().toStdString();
    for (const auto& o : state.value("overlays").toList()) content.overlays.push_back(overlayFrom(o.toMap()));
    std::vector<st::Sticker> candidates;
    for (const auto& v : state.value("stickers").toList()) candidates.push_back(stickerFromMap(v.toMap()));
    content.stickers = st::parseStickers(st::toJson(candidates));
    content.audience = audienceOf(state.value("audience").toString());
    content.customIds = stdList(state.value("customIds").toStringList());
    content.allowReplies = state.value("allowReplies", true).toBool();
    content.ttlSeconds = state.value("ttlSeconds", st::kDefaultStorySeconds).toInt();

    const QString mediaFile = QUrl(state.value("mediaFile").toString()).toLocalFile();
    const bool video = state.value("mediaKind").toString() == QStringLiteral("video");
    const bool withMedia = content.mode == "media" && !mediaFile.isEmpty();
    if (content.mode == "media" && !withMedia) content.mode = "text";
    std::optional<std::string> id;
    if (data::isDraftId(draftIdText.toStdString())) id = draftIdText.toStdString();

    auto me = identity_;
    const data::StoryDraftStore* store = draftStore_.get();
    runInBackground(
        pool_, this,
        [store, me, content, withMedia, mediaFile, video, durationMs, id]() {
            std::optional<data::DraftMediaInput> media;
            if (withMedia) {
                PreparedMedia prepared = prepareMedia(mediaFile, video, durationMs);
                data::DraftMediaInput input;
                input.kind = prepared.kind;
                input.mime = prepared.media.mime;
                input.width = prepared.media.width;
                input.height = prepared.media.height;
                input.durationMs = prepared.media.durationMs;
                input.bytes = std::move(prepared.bytes);
                media = std::move(input);
            }
            return store->save(me->userId, me->privateKey, content, media, id);
        },
        [this](std::string saved) {
            setMessages({}, QStringLiteral("Draft saved on this device"));
            emit draftSaved(qs(saved));
            refreshDrafts();
        },
        [this](const Failure& failure) {
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            setMessages(failure.message.isEmpty() ? QStringLiteral("Couldn't save the draft.") : failure.message, {});
        });
}

void StoriesSession::openDraft(const QString& draftIdText) {
    const std::string id = draftIdText.toStdString();
    auto me = identity_;
    const data::StoryDraftStore* store = draftStore_.get();
    runInBackground(
        pool_, this,
        [store, me, id]() {
            auto loaded = store->load(me->userId, me->privateKey, id);
            if (!loaded) throw api::UserFacingError("That draft can't be opened any more.");
            LoadedDraftResult out;
            if (loaded->media) {
                // The composer shows and posts from a file: a private one, deleted when the composer closes.
                const QString dir = mediaDir();
                QDir().mkpath(dir);
                const std::string& mime = loaded->media->mime;
                const QString ext = mime == "image/png" ? "png" : mime == "image/webp" ? "webp" : mime == "image/jpeg" ? "jpg"
                                  : mime == "video/webm" ? "webm" : mime == "video/quicktime" ? "mov" : "mp4";
                const QString path = dir + u'/' + QString::fromStdString(util::randomUuid()) + u'.' + ext;
                QFile file(path);
                const std::string& bytes = loaded->media->bytes;
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes.data(), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size())) {
                    QFile::remove(path);
                    throw api::UserFacingError("That draft can't be opened any more.");
                }
                file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                out.mediaFile = path;
            }
            out.draft = std::move(*loaded);
            return out;
        },
        [this, draftIdText](LoadedDraftResult r) {
            dropDraftFiles();
            if (!r.mediaFile.isEmpty()) draftFiles_.append(r.mediaFile);
            const auto& c = r.draft.content;
            QVariantList overlays;
            for (const auto& o : c.overlays) overlays.append(overlayMap(o));
            QVariantList stickers;
            for (const auto& s : c.stickers) stickers.append(stickerMap(s));
            const bool media = r.draft.media.has_value();
            emit draftLoaded(QVariantMap{{"id", draftIdText},
                                         {"mode", qs(c.mode)},
                                         {"text", qs(c.text)},
                                         {"background", qs(c.background)},
                                         {"font", qs(c.font)},
                                         {"overlays", overlays},
                                         {"stickers", stickers},
                                         {"audience", qs(c.audience)},
                                         {"customIds", qList(c.customIds)},
                                         {"allowReplies", c.allowReplies},
                                         {"ttlSeconds", c.ttlSeconds},
                                         {"mediaFile", media ? QUrl::fromLocalFile(r.mediaFile).toString() : QString()},
                                         {"mediaKind", media ? qs(r.draft.media->kind) : QString()},
                                         {"durationMs", media && r.draft.media->durationMs ? static_cast<double>(*r.draft.media->durationMs) : 0.0}});
        },
        [this](const Failure& failure) {
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            setMessages(failure.message.isEmpty() ? QStringLiteral("That draft can't be opened any more.") : failure.message, {});
            refreshDrafts();  // an unreadable one has been deleted
        });
}

void StoriesSession::deleteDraft(const QString& draftIdText) {
    const std::string id = draftIdText.toStdString();
    auto me = identity_;
    const data::StoryDraftStore* store = draftStore_.get();
    // Gone from the list at once.
    for (int i = 0; i < drafts_.size(); ++i) {
        if (drafts_[i].toMap().value("id").toString() == draftIdText) {
            drafts_.removeAt(i);
            emit draftsChanged();
            break;
        }
    }
    runInBackground(
        pool_, this, [store, me, id] { store->remove(me->userId, id); }, [this] { refreshDrafts(); }, [](const Failure&) {});
}

// --- Archive and highlights ---

void StoriesSession::loadLibrary() {
    auto api = api_;
    auto me = identity_;
    runInBackground(
        pool_, this,
        [api, me] {
            std::vector<DecodedPtr> out;
            for (const auto& dto : sapi::archive(*api)) {
                if (auto d = decodeStory(dto, me->publicKey, me->privateKey)) {
                    d->seen = true;
                    out.push_back(std::move(d));
                }
            }
            return out;
        },
        [this](std::vector<DecodedPtr> archived) {
            std::vector<DecodedPtr> all = own_;
            for (auto& d : archived) {
                if (std::none_of(all.begin(), all.end(), [&](const DecodedPtr& x) { return x->dto.id == d->dto.id; })) all.push_back(d);
            }
            std::sort(all.begin(), all.end(), [](const DecodedPtr& a, const DecodedPtr& b) { return a->createdMs > b->createdMs; });
            libraryStories_ = all;
            library_.clear();
            for (const auto& d : all) {
                known_[d->dto.id] = d;
                library_.append(storyMap(*d));
            }
            emit libraryChanged();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't load your archive.")); });
    loadHighlights(qs(identity_->userId));
}

QVariantMap StoriesSession::highlightMap(const stories_detail::Highlight& h) const {
    QVariantList stories;
    for (const auto& d : h.stories) stories.append(storyMap(*d));
    return {{"id", qs(h.id)}, {"ownerId", qs(h.ownerId)}, {"title", qs(h.title)}, {"coverStoryId", qs(h.coverStoryId)}, {"stories", stories}};
}

void StoriesSession::loadHighlights(const QString& userIdText) {
    const std::string userId = userIdText.toStdString();
    crypto::Bytes ownerKey;
    if (userId == identity_->userId) {
        ownerKey = identity_->publicKey;
    } else if (const auto f = friends_.find(userId); f != friends_.end()) {
        ownerKey = f->second.publicKey;
    } else {
        emit highlightsLoaded(userIdText, {});
        return;
    }
    auto api = api_;
    auto me = identity_;
    runInBackground(
        pool_, this,
        [api, me, userId, ownerKey] {
            std::vector<stories_detail::Highlight> out;
            for (const auto& h : sapi::highlights(*api, userId)) {
                stories_detail::Highlight view{h.id, h.ownerId, {}, h.coverStoryId, {}};
                for (const auto& dto : h.stories) {
                    auto d = decodeStory(dto, ownerKey, me->privateKey);
                    if (!d || d->dto.authorId != h.ownerId) continue;
                    d->seen = true;
                    view.stories.push_back(std::move(d));
                }
                if (view.stories.empty()) continue;
                const auto cover = std::find_if(view.stories.begin(), view.stories.end(), [&](const DecodedPtr& d) { return d->dto.id == h.coverStoryId; });
                if (cover != view.stories.end()) {
                    try {
                        view.title = sc::openHighlightTitle(crypto::base64Decode(h.titleCiphertext), crypto::base64Decode(h.titleNonce), h.coverStoryId,
                                                            *(*cover)->key);
                    } catch (const std::exception&) {
                    }
                } else {
                    view.coverStoryId = view.stories.front()->dto.id;
                }
                out.push_back(std::move(view));
            }
            return out;
        },
        [this, userIdText, userId](std::vector<stories_detail::Highlight> list) {
            QVariantList out;
            for (auto& h : list) {
                for (const auto& d : h.stories) known_[d->dto.id] = d;
                out.append(highlightMap(h));
                highlights_[h.id] = std::move(h);
            }
            if (userId == identity_->userId) {
                ownHighlights_ = out;
                emit libraryChanged();
            }
            emit highlightsLoaded(userIdText, out);
        },
        [this, userIdText](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            emit highlightsLoaded(userIdText, {});
        });
}

void StoriesSession::saveHighlight(const QString& highlightId, const QString& title, const QStringList& storyIds) {
    std::vector<DecodedPtr> chosen;
    for (const auto& id : storyIds) {
        if (auto d = find(id.toStdString())) chosen.push_back(d);
    }
    const std::string name = title.trimmed().toStdString();
    if (chosen.empty() || name.empty()) {
        setMessages(QStringLiteral("Give it a name and pick at least one story."), {});
        return;
    }
    const DecodedPtr cover = chosen.front();
    // Kept in the order the stories were posted; the first one picked is the cover.
    std::vector<DecodedPtr> ordered = chosen;
    std::sort(ordered.begin(), ordered.end(), [](const DecodedPtr& a, const DecodedPtr& b) { return a->createdMs < b->createdMs; });
    std::vector<std::string> ids;
    for (const auto& d : ordered) ids.push_back(d->dto.id);
    auto api = api_;
    const std::string existing = highlightId.toStdString();
    runInBackground(
        pool_, this,
        [api, existing, name, cover, ids] {
            const auto sealed = sc::sealHighlightTitle(name, cover->dto.id, *cover->key);
            const std::string ct = crypto::base64Encode(sealed.ciphertext);
            const std::string nonce = crypto::base64Encode(sealed.nonce);
            if (existing.empty()) {
                sapi::createHighlight(*api, ct, nonce, cover->dto.id, ids);
            } else {
                sapi::updateHighlight(*api, existing, {ct, nonce, cover->dto.id, ids});
            }
        },
        [this, title] {
            setMessages({}, QStringLiteral("Saved ") + title.trimmed());
            loadHighlights(qs(identity_->userId));
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't save the highlight. Try again.")); });
}

void StoriesSession::addToHighlight(const QString& highlightId, const QString& storyId) {
    const auto it = highlights_.find(highlightId.toStdString());
    if (it == highlights_.end()) return;
    std::vector<std::string> ids;
    for (const auto& d : it->second.stories) ids.push_back(d->dto.id);
    if (has(ids, storyId.toStdString())) return;
    ids.push_back(storyId.toStdString());
    auto api = api_;
    const std::string id = it->first;
    const QString name = qs(it->second.title.empty() ? "highlight" : it->second.title);
    runInBackground(
        pool_, this, [api, id, ids] { sapi::updateHighlight(*api, id, {std::nullopt, std::nullopt, std::nullopt, ids}); },
        [this, name] {
            setMessages({}, QStringLiteral("Added to ") + name);
            loadHighlights(qs(identity_->userId));
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't add it. Try again.")); });
}

void StoriesSession::removeFromHighlight(const QString& highlightId, const QString& storyIdText) {
    const auto it = highlights_.find(highlightId.toStdString());
    if (it == highlights_.end()) return;
    const std::string storyId = storyIdText.toStdString();
    std::vector<DecodedPtr> left;
    for (const auto& d : it->second.stories) {
        if (d->dto.id != storyId) left.push_back(d);
    }
    auto api = api_;
    const std::string id = it->first;
    const bool coverGone = it->second.coverStoryId == storyId;
    const std::string title = it->second.title.empty() ? "Highlight" : it->second.title;
    runInBackground(
        pool_, this,
        [api, id, left, coverGone, title] {
            if (left.empty()) {
                sapi::removeHighlight(*api, id);
                return;
            }
            std::vector<std::string> ids;
            for (const auto& d : left) ids.push_back(d->dto.id);
            if (!coverGone) {
                sapi::updateHighlight(*api, id, {std::nullopt, std::nullopt, std::nullopt, ids});
                return;
            }
            // The cover's key seals the title: a new cover means sealing it again.
            const auto sealed = sc::sealHighlightTitle(title, left.front()->dto.id, *left.front()->key);
            sapi::updateHighlight(*api, id, {crypto::base64Encode(sealed.ciphertext), crypto::base64Encode(sealed.nonce), left.front()->dto.id, ids});
        },
        [this] {
            setMessages({}, QStringLiteral("Removed from highlight"));
            loadHighlights(qs(identity_->userId));
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't change the highlight. Try again.")); });
}

void StoriesSession::deleteHighlight(const QString& highlightId) {
    auto api = api_;
    const std::string id = highlightId.toStdString();
    runInBackground(
        pool_, this, [api, id] { sapi::removeHighlight(*api, id); },
        [this, id] {
            highlights_.erase(id);
            loadHighlights(qs(identity_->userId));
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't delete the highlight. Try again.")); });
}

}  // namespace redvault::app

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "device_keys_session.hpp"

#include <QDateTime>
#include <QFile>
#include <QDesktopServices>
#include <QDir>
#include <QLocale>
#include <QStandardPaths>

#include "async.hpp"

#ifdef REDVAULT_HAS_E2E_V3
#include <map>
#include <mutex>

#include "chat_images.hpp"
#include "chat_models.hpp"
#include "file_safety.hpp"
#include "local_store.hpp"
#include "redvault/model/text_format.hpp"
#include "redvault/e2ev3/key_vault.hpp"
#include "redvault/e2ev3/v3_server.hpp"
#include "redvault/util/text.hpp"
#include "tor_controller.hpp"
#endif

namespace redvault::app {

const QList<QByteArray>& V3ChatModel::names() {
    // MessageDelegate's roles (chat_models.cpp), then this list's own.
    static const QList<QByteArray> kNames = {
        "messageId",   "text",        "fromMe",      "sender",           "showSender",    "timeText",   "dayHeader",
        "kind",        "fileSize",    "fileStatus",  "status",           "queueError",    "queueHeld",  "canDiscard",
        "replyPreview", "replyFromMe", "replyTo",    "reactions",        "verification",  "edited",     "forwarded",
        "unverifiedSender", "storyReply", "richText", "richTextRevealed", "isImage",      "isVoice",    "poll",
        "noticeText",  "channelId",   "channelName", "channelPostId",    "deleted",       "hasFile",    "fileName",
        "canOpen",     "canReact",    "canEdit",     "canDelete",        "canPin",        "pinned",     "time",
        // MessageDelegate requires it (other chats' audio player); device-key voice uses `isVoice`.
        "isAudio",
    };
    return kNames;
}

QVariant V3ChatModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
    const int at = role - (Qt::UserRole + 1);
    if (at < 0 || at >= names().size()) return {};
    return rows_.at(index.row()).toMap().value(QString::fromLatin1(names().at(at)));
}

QHash<int, QByteArray> V3ChatModel::roleNames() const {
    QHash<int, QByteArray> out;
    for (int i = 0; i < names().size(); ++i) out.insert(Qt::UserRole + 1 + i, names().at(i));
    return out;
}

void V3ChatModel::setRows(QVariantList rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
    emit countChanged();
}

int V3ChatModel::indexOf(const QString& messageId) const {
    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_.at(i).toMap().value(QStringLiteral("messageId")).toString() == messageId) return i;
    }
    return -1;
}

int DeviceKeysSession::chatTimer() const {
    return chatId_.isEmpty() ? 0 : openInfo().timerSeconds;
}

DeviceKeysSession::ChatInfo DeviceKeysSession::openInfo() const {
    return chatInfo_ && !chatId_.isEmpty() ? chatInfo_(chatId_.toStdString()) : ChatInfo{};
}

qint64 DeviceKeysSession::voicePosition() const {
    return voicePlayer_ ? voicePlayer_->position() : 0;
}

qint64 DeviceKeysSession::voiceDuration() const {
    return voicePlayer_ ? voicePlayer_->duration() : 0;
}

bool DeviceKeysSession::voicePlaying() const {
    return voicePlayer_ && voicePlayer_->playbackState() == QMediaPlayer::PlayingState;
}

#ifdef REDVAULT_HAS_E2E_V3

namespace {

using e2ev3::Json;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

QString str(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? qs(it->get<std::string>()) : QString();
}

/// What each security event means to the person reading it (the web panel's wording).
QString eventText(const QString& kind) {
    static const QHash<QString, QString> text = {
        {"device_registered", "A device registered its keys (trusted only once approved)"},
        {"device_list_created", "Device keys were set up for this account"},
        {"device_added", "A device was approved"},
        {"device_removed", "A device was removed"},
        {"device_unregistered", "A device's keys were deleted"},
        {"enrollment_requested", "A new device asked to pair"},
        {"account_key_rotated", "The account key was replaced by a trusted device"},
        {"account_reset_requested", "Someone asked to reset this account's keys"},
        {"account_reset_cancelled", "A reset of this account's keys was cancelled"},
        {"account_reset_completed", "This account's keys were reset"},
        {"recovery_backup_updated", "A recovery backup was saved"},
        {"recovery_backup_downloaded", "The recovery backup was downloaded"},
        {"signal_enabled", "A device turned on Signal sessions"},
        // Noticed on this computer, not by the server (DeviceStore's rollback check).
        {"state_rolled_back", "This device's keys were restored from an older copy and were turned off. Link this computer again."},
        {"state_unverified", "This device's stored keys didn't check out and were turned off. Link this computer again."},
        {"counter_missing", "This device's keys couldn't be checked against their saved counter and were turned off. Link this computer again."},
    };
    return text.value(kind, kind);
}

/// An engine refusal worded for the person; the code stays available to the caller.
QString friendly(const e2ev3::EngineError& error) {
    const std::string& code = error.code();
    if (code == "bad_device_list" || code == "equivocation" || code == "rollback") {
        return QStringLiteral("This account's device list doesn't verify. Don't approve anything until you know why.");
    }
    if (code == "account_key_changed" || code == "uncertified_account_key") {
        return QStringLiteral("The account's device keys changed. Compare safety numbers before trusting the new keys.");
    }
    if (code == "bad_recovery_key" || code == "bad_backup") {
        return QStringLiteral("That recovery key doesn't open this account's backup. A recovery key has 64 letters and digits.");
    }
    if (code == "pairing_timeout") return QStringLiteral("The other device didn't answer in time. Start again.");
    if (code == "bad_commitment" || code == "bad_pairing") {
        return QStringLiteral("The pairing didn't check out, so nothing was approved. Start again.");
    }
    if (code == "legacy_key_mismatch") return QStringLiteral("The recovery backup doesn't match this account's current key.");
    return QString::fromUtf8(error.what());
}

/// Converts an engine refusal into an error the shared handler words for the UI.
template <typename F>
auto translated(F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const e2ev3::EngineError& error) {
        if (error.code() == "cancelled") throw api::UserFacingError("");
        throw api::UserFacingError(friendly(error).toStdString());
    }
}

QString pairKind(e2ev3::Protection p) {
    switch (p) {
        case e2ev3::Protection::Dpapi:
            return QStringLiteral("dpapi");
        case e2ev3::Protection::SecretService:
            return QStringLiteral("keyring");
        case e2ev3::Protection::File:
            return QStringLiteral("file");
    }
    return QStringLiteral("file");
}

e2ev3::WaitOptions waitFor(const std::shared_ptr<std::atomic<bool>>& stop) {
    e2ev3::WaitOptions wait;
    wait.cancelled = [stop] { return stop->load(); };
    return wait;
}

bool isImageMime(const std::string& mime) {
    const QString m = qs(mime).toLower();
    // The decoder checks the bytes themselves; this only picks what to try (as protocol-1 chats do).
    return m == QStringLiteral("image/jpeg") || m == QStringLiteral("image/png") || m == QStringLiteral("image/gif") ||
           m == QStringLiteral("image/webp") || m == QStringLiteral("image/bmp");
}

bool isAudioMime(const std::string& mime) {
    return qs(mime).toLower().startsWith(QStringLiteral("audio/"));
}

/// What a reply quotes, or a pin bar shows, for a message: its text without markers, a file's name, a poll's question.
std::string previewFor(const e2ev3::ChatRecord& r) {
    if (r.deleted) return {};
    if (r.file) return "\xF0\x9F\x93\x8E " + r.file->filename;  // paperclip
    if (r.poll) return "\xF0\x9F\x93\x8A " + r.text;  // bar chart
    return model::text_format::toPlain(r.text);
}

/// What a row needs besides its own record.
struct RowContext {
    const e2ev3::ChatRecord* previous = nullptr;
    const std::map<std::string, const e2ev3::ChatRecord*>* byId = nullptr;
    const DeviceKeysSession::ChatInfo* info = nullptr;
    std::string selfUserId;
    std::int64_t now = 0;
};

/**
 * One chat row with the roles MessageDelegate reads for a protocol-1 message (chat_models.cpp), so a
 * device-key chat looks and works like the others: formatting, quotes, polls, ticks, pictures. Only
 * the actions that message allows are offered.
 */
QVariantMap chatRow(const e2ev3::ChatRecord& r, const RowContext& c) {
    constexpr std::int64_t kEditWindow = 24LL * 3600 * 1000;  // Signal's window, like the other chats
    const DeviceKeysSession::ChatInfo& info = *c.info;
    const bool mine = r.direction == "out";
    // A queued one (waiting for Tor) hasn't reached anyone yet either.
    const bool failed = r.status == "failed";
    const bool waiting = r.status == "queued";
    std::map<QString, std::pair<int, bool>> grouped;
    for (const e2ev3::ChatReaction& reaction : r.reactions) {
        auto& held = grouped[qs(reaction.emoji)];
        ++held.first;
        held.second = held.second || reaction.userId == c.selfUserId;
    }
    QVariantList reactions;
    for (const auto& [emoji, held] : grouped) {
        reactions.append(QVariantMap{{"emoji", emoji}, {"count", held.first}, {"mine", held.second}});
    }
    // Part D: a reply or reaction to a story, worded as in the other chats.
    QString storyReplyText;
    if (r.storyReply) {
        const auto& sr = *r.storyReply;
        const QString what = mine ? (sr.reaction ? QStringLiteral("You reacted to their story") : QStringLiteral("You replied to their story"))
                                  : (sr.reaction ? QStringLiteral("Reacted to your story") : QStringLiteral("Replied to your story"));
        storyReplyText = what + QStringLiteral(" ") + QChar(0x00B7) + QStringLiteral(" ") + qs(sr.preview);
    }
    const auto handleOf = [&](const std::string& userId) -> QString {
        if (userId == c.selfUserId) return QStringLiteral("You");
        const auto it = info.handles.find(userId);
        return it != info.handles.end() && !it->second.empty() ? qs(it->second) : QStringLiteral("Someone");
    };
    // Ticks on your own messages, from the receipts: delivered or read by everyone else in the chat.
    QString status;
    if (mine) {
        if (failed || waiting) {
            status = QStringLiteral("queued");
        } else {
            // Delivered once anyone has a receipt, read once anyone has read it (the same rule on every client).
            const bool read = std::any_of(r.receipts.begin(), r.receipts.end(), [](const e2ev3::ChatReceipt& x) { return x.status == "read"; });
            status = read ? QStringLiteral("seen") : !r.receipts.empty() ? QStringLiteral("delivered") : QStringLiteral("sent");
        }
    }
    const QString fileName = r.file ? qs(r.file->filename) : QString();
    const QString kind = r.poll ? QStringLiteral("poll") : r.file ? QStringLiteral("file") : QStringLiteral("text");
    QString text = r.deleted ? QStringLiteral("This message was deleted.") : r.file ? fileName : qs(r.text);
    QString richText;
    QString richRevealed;
    if (!r.deleted && !r.file && !r.poll) {
        model::text_format::Options options;
        if (info.isGroup) {
            for (const auto& [user, handle] : info.handles) options.mentions.push_back(handle);
            options.selfHandle = info.selfHandle;
        }
        richText = qs(model::text_format::toHtml(r.text, options));
        options.revealSpoilers = true;
        richRevealed = qs(model::text_format::toHtml(r.text, options));
    }
    QVariantMap poll;
    if (r.poll && !r.deleted) {
        std::vector<int> picked;
        int voters = 0;
        for (const auto& [user, choices] : r.poll->ballots) {
            ++voters;
            if (user == c.selfUserId) picked = choices;
        }
        QVariantList options;
        for (std::size_t i = 0; i < r.poll->options.size(); ++i) {
            options.append(QVariantMap{{"label", qs(r.poll->options[i])},
                                       {"count", i < r.poll->votes.size() ? r.poll->votes[i] : 0},
                                       {"mine", std::find(picked.begin(), picked.end(), static_cast<int>(i)) != picked.end()}});
        }
        poll = QVariantMap{{"question", qs(r.text)}, {"multiple", r.poll->multi}, {"closed", r.poll->ended},
                           {"voters", voters},       {"voted", !picked.empty()}, {"options", options}};
    }
    bool replyFromMe = false;
    QString replyPreview = qs(r.replyPreview);
    if (!r.replyTo.empty()) {
        if (const auto it = c.byId->find(r.replyTo); it != c.byId->end()) {
            replyFromMe = it->second->from == c.selfUserId;
            if (it->second->deleted) replyPreview = QStringLiteral("Deleted message");
            else if (replyPreview.isEmpty()) replyPreview = qs(previewFor(*it->second));
        } else if (replyPreview.isEmpty()) {
            replyPreview = QStringLiteral("A message that isn't on this computer");
        }
    }
    const auto local = [](std::int64_t ms) { return QDateTime::fromMSecsSinceEpoch(ms).toLocalTime(); };
    const bool sameDay = c.previous != nullptr && local(c.previous->ts).date() == local(r.ts).date();
    const bool showSender = info.isGroup && !mine && (c.previous == nullptr || c.previous->from != r.from || !sameDay);
    QString timeText = formatClock(r.ts);
    if (r.expiresAt) timeText = QStringLiteral("\u23F2 ") + timeText;  // it disappears
    const bool usable = !failed && !waiting && !r.deleted;
    return QVariantMap{{"messageId", qs(r.id)},
                       {"text", text},
                       {"fromMe", mine},
                       {"sender", handleOf(r.from)},
                       {"showSender", showSender},
                       {"timeText", timeText},
                       {"dayHeader", sameDay ? QString() : formatDay(r.ts)},
                       {"kind", kind},
                       {"fileSize", r.file ? qs(model::file_message::formatBytes(r.file->originalSize)) : QString()},
                       {"fileStatus", QString()},
                       {"status", status},
                       {"queueError", failed ? QStringLiteral("It couldn't be sent. Retry it, or discard it.")
                                             : waiting ? QStringLiteral("Waiting for Tor. It's kept encrypted on this computer.") : QString()},
                       {"queueHeld", failed},
                       {"canDiscard", failed},
                       {"replyPreview", replyPreview},
                       {"replyFromMe", replyFromMe},
                       {"replyTo", qs(r.replyTo)},
                       {"reactions", reactions},
                       {"verification", QString()},
                       {"edited", r.edited},
                       {"forwarded", r.forwarded},
                       {"unverifiedSender", false},
                       {"storyReply", storyReplyText},
                       {"isAudio", false},
                       {"richText", richText},
                       {"richTextRevealed", richRevealed},
                       {"isImage", usable && r.file && isImageMime(r.file->mimeType)},
                       {"isVoice", usable && r.file && isAudioMime(r.file->mimeType)},
                       {"poll", poll},
                       {"noticeText", QString()},
                       {"channelId", QString()},
                       {"channelName", QString()},
                       {"channelPostId", QString()},
                       {"deleted", r.deleted},
                       {"hasFile", r.file.has_value()},
                       {"fileName", fileName},
                       {"canOpen", r.file.has_value() && file_safety::isSafeToOpen(fileName)},
                       // A failed send never reached anyone, so nothing can react to it, pin it or edit it.
                       {"canReact", usable},
                       {"canEdit", mine && usable && !r.file && !r.poll && c.now - r.ts < kEditWindow},
                       {"canDelete", mine && !failed && !waiting && !r.deleted},
                       {"canPin", usable},
                       {"pinned", r.pinned},
                       {"time", QLocale().toString(local(r.ts), QLocale::ShortFormat)}};
}

}  // namespace

struct DeviceKeysSession::Snapshot {
    bool enabled = false;
    QString status = QStringLiteral("off");
    std::shared_ptr<e2ev3::V3Device> device;
    QVariantList devices;
    QVariantList waiting;
    QVariantMap pendingReset;
    QVariantList events;
    QVariantMap protection;
    bool interrupted = false;
};

namespace {

std::mutex& storeMutex() {
    static std::mutex m;
    return m;
}

/// A call envelope has this long to go out before the call fails (docs/signal-integration/09-part-b-calls.md).
constexpr int kCallSendTimeoutMs = 30000;

}  // namespace

/// Call envelopes the chat thread received (any receive pass), until the UI thread checks and hands them out.
struct DeviceKeysSession::CallInbox {
    std::mutex mutex;
    std::vector<e2ev3::CallMessage> items;
    std::function<void()> notify;  // asks the UI thread to dispatch
    // Part D: story keys and named answers go into this device's story store on every receive pass.
    std::shared_ptr<e2ev3::StoryStore> stories;
    std::vector<std::string> storyKeys;  // stories whose key just arrived, for the UI thread
    std::function<void()> notifyStories;
};

/// The device and its story store, for the stories thread (the UI thread replaces them).
struct DeviceKeysSession::StoryPart {
    std::mutex mutex;
    std::shared_ptr<e2ev3::V3Device> device;
    std::shared_ptr<e2ev3::StoryStore> store;
};

#endif

DeviceKeysSession::DeviceKeysSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, QObject* parent)
    : QObject(parent), api_(std::move(api)), identity_(std::move(identity)) {
    pool_.setMaxThreadCount(1);
    waitPool_.setMaxThreadCount(2);
    chatPool_.setMaxThreadCount(1);
    callPool_.setMaxThreadCount(2);
    receiveTimer_.setInterval(30000);
    connect(&receiveTimer_, &QTimer::timeout, this, &DeviceKeysSession::receiveNow);
#ifdef REDVAULT_HAS_E2E_V3
    callInbox_ = std::make_shared<CallInbox>();
    callInbox_->notify = [this] { QMetaObject::invokeMethod(this, [this] { dispatchCalls(); }, Qt::QueuedConnection); };
    callInbox_->notifyStories = [this] { QMetaObject::invokeMethod(this, [this] { announceStoryKeys(); }, Qt::QueuedConnection); };
    storyPart_ = std::make_shared<StoryPart>();
    // Receipts go in batches (part A), and disappearing messages are erased when their time comes.
    receiptTimer_.setSingleShot(true);
    connect(&receiptTimer_, &QTimer::timeout, this, &DeviceKeysSession::flushReceipts);
    expiryTimer_.setSingleShot(true);
    connect(&expiryTimer_, &QTimer::timeout, this, &DeviceKeysSession::eraseExpiredNow);
#endif
    // The background "ensure device keys" check, every 6 hours while the app runs (and after sign-in and unlock).
    ensureTimer_.setInterval(6 * 60 * 60 * 1000);
    connect(&ensureTimer_, &QTimer::timeout, this, &DeviceKeysSession::ensureDeviceKeys);
    if (available()) ensureTimer_.start();
#ifdef REDVAULT_HAS_E2E_V3
    // The app's signed-in ApiClient: its one transport goes only to our server, and only through Tor in a Tor build.
    queueing_ = std::make_shared<e2ev3::QueueingV3Server>(std::make_shared<e2ev3::HttpV3Server>(api_));
    server_ = queueing_;
    // Part C2: communities over device keys; their routes use the same ApiClient (the Tor transport).
    communities_ = std::make_shared<e2ev3::Communities>(std::make_shared<e2ev3::HttpCommunityDirectory>(api_));
    // Owner and staff devices look after membership (admit, rosters, sender-key rotation) every 30 s.
    membershipTimer_.setInterval(30000);
    connect(&membershipTimer_, &QTimer::timeout, this, &DeviceKeysSession::runMembershipTasks);
    // Decrypted files go in a folder of their own for this session, removed at sign-out. No conversation
    // repository: nothing here goes through a protocol-1 message.
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    cacheDir_ = std::filesystem::path(cacheRoot.toStdWString()) / ("session-" + util::randomUuid());
    files_ = std::make_shared<data::FileRepository>(api_, nullptr, cacheDir_);
#endif
}

DeviceKeysSession::~DeviceKeysSession() {
#ifdef REDVAULT_HAS_E2E_V3
    stopWaiting_->store(true);
#endif
    receiveTimer_.stop();
    ensureTimer_.stop();
    chatPool_.clear();
    chatPool_.waitForDone();
    callPool_.clear();
    callPool_.waitForDone();
    pool_.clear();
    waitPool_.clear();
    pool_.waitForDone();
    waitPool_.waitForDone();
#ifdef REDVAULT_HAS_E2E_V3
    std::error_code ignored;
    std::filesystem::remove_all(cacheDir_, ignored);
#endif
}

void DeviceKeysSession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit changed();
}

void DeviceKeysSession::setError(const QString& message) {
    if (error_ == message) return;
    error_ = message;
    emit changed();
}

void DeviceKeysSession::clearError() {
    setError({});
}

bool DeviceKeysSession::ready() const {
    return available() && status_ != QStringLiteral("off");
}

void DeviceKeysSession::dismissRecoveryNotice() {
    if (!recoveryNotice_) return;
    recoveryNotice_ = false;
    emit changed();
}

void DeviceKeysSession::setChatMode(const QString& mode) {
    const bool active = mode == QStringLiteral("device");
    if (chatMode_ == mode && chatActive_ == active) return;
    const bool wasActive = chatActive_;
    chatMode_ = mode;
    chatActive_ = active;
    receiveTimer_.setInterval(active ? 5000 : 30000);
    if (active && !wasActive) chatError_.clear();
    emit chatChanged();
    if (active && !wasActive) receiveNow();
}

bool DeviceKeysSession::callsOverDeviceKeys(const QString& chatId, QString* refusal) {
    if (!ready() || chatId.isEmpty() || !chatPinned(chatId)) return false;
    // The chat is on device keys (or not decided yet): never the older protocol for its calls.
    QString why = chatRefusal(chatId);
    if (why.isEmpty() && !trusted()) {
        why = QStringLiteral("This computer isn't linked to your devices yet, so the call can't be set up. Link it in Settings > Devices & encryption.");
    }
    if (!why.isEmpty()) {
        why.replace(QStringLiteral("so nothing was sent"), QStringLiteral("so the call wasn't started"));
        if (refusal) *refusal = why;
        return false;
    }
    return true;
}

QString DeviceKeysSession::chatRefusal(const QString& chatId) const {
    if (chatId.isEmpty() || chatId != chatId_) return {};
    if (chatMode_ == QStringLiteral("checking")) return QStringLiteral("Checking how this chat is encrypted, so nothing was sent. Try again in a moment.");
    if (chatMode_ == QStringLiteral("link")) {
        return QStringLiteral("This computer isn't linked to your devices yet, so nothing was sent. Link it in Settings > Devices & encryption.");
    }
    return {};
}

#ifndef REDVAULT_HAS_E2E_V3

void DeviceKeysSession::refresh() {}
void DeviceKeysSession::refreshWith(bool) {}
void DeviceKeysSession::ensureDeviceKeys() {}
void DeviceKeysSession::decideChat() {}
void DeviceKeysSession::turnOffSignal(const QString&) {}
void DeviceKeysSession::setUp() {}
void DeviceKeysSession::startPairing() {}
void DeviceKeysSession::confirmJoin() {}
void DeviceKeysSession::cancelJoin() {}
void DeviceKeysSession::compareCodes(const QString&) {}
void DeviceKeysSession::approve() {}
void DeviceKeysSession::rejectAnswer() {}
void DeviceKeysSession::revoke(const QString&) {}
void DeviceKeysSession::removeWaiting(const QString&) {}
void DeviceKeysSession::createRecoveryKey() {}
void DeviceKeysSession::dismissRecoveryKey() {}
void DeviceKeysSession::restore(const QString&) {}
void DeviceKeysSession::cancelReset() {}
void DeviceKeysSession::forgetDevice() {}
void DeviceKeysSession::startOver() {}
void DeviceKeysSession::loadSafetyNumber(const QString& contactUserId, const QString&) {
    emit safetyNumberReady(contactUserId, {});
}
void DeviceKeysSession::setDeviceKeysVerified(const QString& contactUserId, const QString&, const QString&, bool) {
    emit safetyNumberReady(contactUserId, {});
}
void DeviceKeysSession::acceptNewDeviceKeys(const QString& contactUserId, const QString&, const QString&, bool) {
    emit safetyNumberReady(contactUserId, {});
}

bool DeviceKeysSession::chatPinned(const QString&) {
    return false;
}

void DeviceKeysSession::openChat(const QString&, const QString&, bool, bool) {}
void DeviceKeysSession::closeChat() {}
void DeviceKeysSession::useOlderProtocol() {}
void DeviceKeysSession::sendChat(const QString&, const QString&) {}
void DeviceKeysSession::retryChat(const QString&) {}
void DeviceKeysSession::reactChat(const QString&, const QString&) {}
void DeviceKeysSession::editChat(const QString&, const QString&) {}
void DeviceKeysSession::deleteChat(const QString&) {}
void DeviceKeysSession::sendFile(const QUrl&) {}
void DeviceKeysSession::openFile(const QString&) {}
void DeviceKeysSession::saveFile(const QString&, const QUrl&) {}
void DeviceKeysSession::receiveNow() {}
void DeviceKeysSession::clearChatError() {}
QString DeviceKeysSession::previewOf(const QString&) {
    return {};
}
void DeviceKeysSession::createPoll(const QString&, const QStringList&, bool) {}
void DeviceKeysSession::vote(const QString&, const QVariantList&) {}
void DeviceKeysSession::endPoll(const QString&) {}
void DeviceKeysSession::pinChat(const QString&, bool) {}
void DeviceKeysSession::markShown(const QString&) {}
QStringList DeviceKeysSession::forwardable(const QStringList&) {
    return {};
}
bool DeviceKeysSession::anyFile(const QStringList&) {
    return false;
}
bool DeviceKeysSession::allFromMe(const QStringList&) {
    return false;
}
void DeviceKeysSession::deleteMany(const QStringList&) {}
void DeviceKeysSession::discardChat(const QString&) {}
QVariantMap DeviceKeysSession::imageInfo(const QString&) const {
    return {};
}
void DeviceKeysSession::loadImage(const QString&, bool) {}
void DeviceKeysSession::toggleVoice(const QString&) {}
std::optional<DeviceKeysSession::ForwardItem> DeviceKeysSession::forwardItem(const QString&) {
    return std::nullopt;
}
void DeviceKeysSession::forwardInto(const QString&, std::vector<ForwardItem>) {}
void DeviceKeysSession::setTor(TorController* tor) {
    tor_ = tor;
}
void DeviceKeysSession::expectCall(const std::string&, const std::string&, const std::string&, bool) {}
void DeviceKeysSession::expectGroupCall(const std::string&, const std::string&) {}
void DeviceKeysSession::forgetCall(const std::string&) {}
void DeviceKeysSession::setGroupCallMembers(const std::string&, const std::string&, const std::vector<std::string>&) {}
void DeviceKeysSession::setCommunities(const std::vector<CommunityChat>&) {}
void DeviceKeysSession::membershipNow() {}
std::string DeviceKeysSession::callPeerDevice(const std::string&) const {
    return {};
}
void DeviceKeysSession::sendCallSignal(const std::string&, const std::string&, const std::string&, const std::string&, const std::string&,
                                       const std::string&, const std::string&, std::function<void(const QString&)> done) {
    if (done) done(QStringLiteral("Device keys aren't part of this build."));
}
void DeviceKeysSession::sendCallKey(const std::string&, const std::string&, std::int64_t, std::shared_ptr<const crypto::SecretBytes>,
                                    std::vector<std::string>, std::function<void(const QString&)> done) {
    if (done) done(QStringLiteral("Device keys aren't part of this build."));
}
bool DeviceKeysSession::storiesOnDeviceKeys() const {
    return false;
}
std::shared_ptr<const crypto::SecretBytes> DeviceKeysSession::storyKey(const std::string&, const std::string&) {
    return nullptr;
}
void DeviceKeysSession::forgetStory(const std::string&) {}
std::vector<StoryDeviceKeys::HeldKey> DeviceKeysSession::heldStoryKeys() {
    return {};
}
std::vector<StoryDeviceKeys::NamedAnswer> DeviceKeysSession::storyAnswers(const std::string&) {
    return {};
}
void DeviceKeysSession::sendStoryKey(const std::string&, std::shared_ptr<const crypto::SecretBytes>, std::int64_t, std::vector<std::string>,
                                     std::function<void(const QString&)> done) {
    if (done) done(QStringLiteral("Device keys aren't part of this build."));
}
void DeviceKeysSession::sendStoryAnswer(const std::string&, const std::string&, const std::string&, bool, std::string, std::int64_t,
                                        std::function<void(const QString&)> done) {
    if (done) done(QStringLiteral("Device keys aren't part of this build."));
}
void DeviceKeysSession::fillStoryGaps(std::vector<std::pair<std::string, std::vector<std::string>>>,
                                      std::function<void(const std::string&, const std::vector<std::string>&)>) {}
crypto::Bytes DeviceKeysSession::sealStoryAnswer(const std::string&, const std::string&, const model::stories::Sticker&, const model::stories::Answer&) {
    throw std::runtime_error("Device keys aren't part of this build.");
}
std::optional<model::stories::Answer> DeviceKeysSession::openStoryAnswer(const std::string&, const model::stories::Sticker&, const crypto::Bytes&) {
    return std::nullopt;
}
void DeviceKeysSession::sendStoryReply(const std::string&, const std::string&, const std::string&, const model::StoryReplyRef&,
                                       std::function<void(const QString&)> done) {
    if (done) done(QStringLiteral("Device keys aren't part of this build."));
}

#else

e2ev3::Backend DeviceKeysSession::backend() {
    std::lock_guard lock(storeMutex());
    // Made on first use, off the UI thread: finding a keyring can take a moment (and may ask for it to be unlocked).
    if (!store_) {
        const std::filesystem::path root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toStdString();
        const auto folder = root / "device-keys";
        store_ = std::make_shared<e2ev3::DeviceStore>(folder / "state", std::shared_ptr<e2ev3::KeyVault>(e2ev3::makeDefaultVault(folder / "keys")));
    }
    return e2ev3::Backend{store_, server_, communities_};
}

void DeviceKeysSession::apply(const Snapshot& s) {
    enabled_ = s.enabled;
    status_ = s.status;
    if (s.device) device_ = s.device;
    devices_ = s.devices;
    waiting_ = s.waiting;
    pendingReset_ = s.pendingReset;
    events_ = s.events;
    protection_ = s.protection;
    interrupted_ = s.interrupted;
    known_ = true;
    if (status_ == QStringLiteral("trusted")) {
        if (!receiveTimer_.isActive()) receiveTimer_.start();
        if (!membershipTimer_.isActive()) {
            membershipTimer_.start();
            QTimer::singleShot(0, this, &DeviceKeysSession::runMembershipTasks);  // on start too
        }
    } else {
        receiveTimer_.stop();
        membershipTimer_.stop();
    }
    emit changed();
    decideChat();
}

void DeviceKeysSession::run(const char*, std::function<void()> work, std::function<void()> done) {
    if (busy_) return;
    setBusy(true);
    setError({});
    runInBackground(
        pool_, this, [work = std::move(work)] { translated(work); },
        [this, done = std::move(done)]() {
            setBusy(false);
            if (done) done();
            refresh();
        },
        [this](const Failure& failure) {
            setBusy(false);
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            setError(failure.message);
            refresh();
        });
}

void DeviceKeysSession::refresh() {
    refreshWith(false);
}

void DeviceKeysSession::ensureDeviceKeys() {
    if (!available()) return;
    // Only while signed in and unlocked: the unlock runs it again.
    if (locked_ && locked_()) return;
    ensurePending_ = true;
    refreshWith(true);
}

void DeviceKeysSession::refreshWith(bool quiet) {
    if (!available() || refreshing_) return;
    refreshing_ = true;
    const std::string userId = identity_->userId;
    auto current = device_;
    auto self = this;
    // This computer's device, opened from disk before anything is asked of the server: a pinned chat stays
    // pinned even when the server can't be reached.
    auto opened = std::make_shared<std::shared_ptr<e2ev3::V3Device>>();
    runInBackground(
        pool_, this,
        [self, userId, current, opened]() -> Snapshot {
            return translated([&]() -> Snapshot {
                Snapshot s;
                const e2ev3::Backend b = self->backend();
                s.protection = QVariantMap{{"kind", pairKind(b.store->protection())},
                                           {"text", qs(e2ev3::describeProtection(b.store->protection()))},
                                           {"weak", e2ev3::isWeakProtection(b.store->protection())}};
                std::shared_ptr<e2ev3::V3Device> device = current;
                if (!device) device = e2ev3::V3Device::open(userId, b);
                *opened = device;
                const Json config = b.server->config();
                s.enabled = config.value("enabled", false);
                if (!s.enabled) return s;
                s.device = device;
                const Json view = b.server->getDeviceList(userId);
                Json list = view.value("deviceList", Json(nullptr));
                bool linked = false;
                bool trusted = false;
                bool listedHere = false;
                if (device) {
                    linked = device->op("deviceList").value("ok", false);
                    if (linked && !list.is_null()) {
                        const Json checked = device->op("verifyDeviceList", Json{{"deviceList", list}});
                        if (!checked.value("ok", false)) throw e2ev3::EngineError("bad_device_list", "This account's device list doesn't verify.");
                        listedHere = checked.value("thisDeviceListed", true);
                        const Json own = device->op("deviceList");
                        trusted = own.value("ok", false) && own.value("trusted", false);
                        if (own.contains("deviceList") && own.at("deviceList").is_object()) list = own.at("deviceList");
                    }
                }
                const Json pending = view.value("pendingReset", Json(nullptr));
                if (pending.is_object()) s.pendingReset = QVariantMap{{"effectiveAt", str(pending, "effectiveAt")}};

                const Json registered = b.server->listDevices();
                Json enrollments = Json::array();
                if (device && trusted) enrollments = b.server->listEnrollments();
                const Json events = b.server->events();

                const std::string thisId = device ? device->deviceId() : std::string();
                if (list.is_object()) {
                    for (const Json& d : list.value("devices", Json::array())) {
                        const std::string id = d.value("deviceId", std::string());
                        s.devices.append(QVariantMap{{"deviceId", qs(id)},
                                                     {"fingerprint", qs(d.value("ed25519", std::string()).substr(0, 16)) + QStringLiteral("…")},
                                                     {"isThis", id == thisId},
                                                     {"canRemove", trusted && id != thisId},
                                                     {"signal", [&] {
                                                          for (const Json& c : list.value("capabilities", Json::array())) {
                                                              if (c.value("deviceId", std::string()) == id) return true;
                                                          }
                                                          return false;
                                                      }()}});
                    }
                }
                for (const Json& d : registered) {
                    const std::string id = d.value("deviceId", std::string());
                    if (d.value("listed", false) || id == thisId) continue;
                    QString enrollmentId;
                    for (const Json& e : enrollments) {
                        if (e.value("newDeviceId", std::string()) == id) enrollmentId = qs(e.value("id", std::string()));
                    }
                    s.waiting.append(QVariantMap{{"deviceId", qs(id)}, {"enrollmentId", enrollmentId}, {"asking", !enrollmentId.isEmpty()}});
                }
                // What this computer noticed itself (its keys restored from an older copy) comes first: it's why it isn't linked.
                const std::vector<e2ev3::StoreEvent> local = b.store->events(userId);
                for (auto it = local.rbegin(); it != local.rend() && s.events.size() < 3; ++it) {
                    s.events.append(QVariantMap{{"text", eventText(qs(it->kind))},
                                                {"when", QDateTime::fromMSecsSinceEpoch(it->atMs).toUTC().toString(Qt::ISODateWithMs)}});
                }
                int shown = 0;
                for (auto it = events.rbegin(); it != events.rend() && shown < 8; ++it, ++shown) {
                    s.events.append(QVariantMap{{"text", eventText(str(*it, "kind"))}, {"when", str(*it, "createdOn")}});
                }

                if (!device && list.is_null()) {
                    s.status = QStringLiteral("setup");
                } else if (!device || !linked) {
                    s.status = QStringLiteral("pair");
                    s.interrupted = device != nullptr;
                } else if (!listedHere) {
                    s.status = QStringLiteral("removed");
                } else {
                    s.status = trusted ? QStringLiteral("trusted") : QStringLiteral("pair");
                }
#ifdef REDVAULT_HAS_E2E_SIGNAL
                // rv-signal-v1: turn it on, declare it in the signed list, top keys up.
                // A failure here never blocks the rest of the device-key screen; the next refresh tries again.
                if (device && linked && listedHere) {
                    try {
                        e2ev3::maintainSignal(*device, config);
                    } catch (const std::exception&) {
                    }
                }
#endif
                return s;
            });
        },
        [this](Snapshot s) {
            refreshing_ = false;
            apply(s);
            if (ensurePending_) {
                ensurePending_ = false;
                if (status_ == QStringLiteral("setup")) autoSetUp();
            }
        },
        [this, quiet, opened](const Failure& failure) {
            refreshing_ = false;
            ensurePending_ = false;
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            // The server answering 404/503 for protocol 3 means it is off: nothing to show, nothing to report.
            if (failure.status == 404 || failure.status == 503) {
                status_ = QStringLiteral("off");
                known_ = true;
                emit changed();
                decideChat();
                return;
            }
            // Anything else (no connection, a server error) is never taken as "off": what was known stays,
            // and the next check tries again. The device opened from disk is kept, for its pinned chats.
            if (!device_ && *opened) device_ = *opened;
            decideChat();
            if (!quiet) setError(failure.message);
            // Until the first answer, chats wait on it: ask again soon rather than at the next 6-hour check.
            if (!known_) QTimer::singleShot(30000, this, [this] { refreshWith(true); });
        });
}

void DeviceKeysSession::autoSetUp() {
    if (status_ != QStringLiteral("setup") || autoSettingUp_ || busy_ || device_) return;
    autoSettingUp_ = true;
    // Busy, so Settings' own "Set up" (which would replace this device half way) waits for it.
    setBusy(true);
    const std::string userId = identity_->userId;
    const auto key = identity_->privateKey.view();
    auto legacy = std::make_shared<crypto::Bytes>(key.begin(), key.end());
    auto self = this;
    auto made = std::make_shared<std::shared_ptr<e2ev3::V3Device>>();
    runInBackground(
        pool_, this,
        [self, userId, legacy, made] {
            try {
                translated([&] { *made = e2ev3::setUpFirstDevice(userId, *legacy, self->backend()); });
            } catch (...) {
                std::fill(legacy->begin(), legacy->end(), 0);
                throw;
            }
            std::fill(legacy->begin(), legacy->end(), 0);
        },
        [this, made] {
            autoSettingUp_ = false;
            setBusy(false);
            device_ = *made;
            recoveryNotice_ = true;
            emit changed();
            // Signal next (maintainSignal runs with the refresh), quietly.
            refreshWith(true);
        },
        [this](const Failure& failure) {
            autoSettingUp_ = false;
            setBusy(false);
            // Silent: the next check (unlock, the 6-hour timer, the next sign-in) tries again.
            if (failure.status == 401) emit sessionExpired();
        });
}

void DeviceKeysSession::setUp() {
    if (status_ != QStringLiteral("setup") || autoSettingUp_ || device_) return;
    const std::string userId = identity_->userId;
    const auto key = identity_->privateKey.view();
    auto legacy = std::make_shared<crypto::Bytes>(key.begin(), key.end());
    auto self = this;
    auto made = std::make_shared<std::shared_ptr<e2ev3::V3Device>>();
    run(
        "setUp",
        [self, userId, legacy, made] {
            *made = e2ev3::setUpFirstDevice(userId, *legacy, self->backend());
            std::fill(legacy->begin(), legacy->end(), 0);
        },
        [this, made] {
            device_ = *made;
            recoveryNotice_ = true;
        });
}

void DeviceKeysSession::startPairing() {
    if (status_ != QStringLiteral("pair") || joining_ || device_) return;
    const std::string userId = identity_->userId;
    auto self = this;
    stopWaiting_->store(false);
    setError({});
    auto made = std::make_shared<std::shared_ptr<e2ev3::V3Device>>();
    auto pairing = std::make_shared<std::shared_ptr<e2ev3::NewDevicePairing>>();
    setBusy(true);
    runInBackground(
        waitPool_, this,
        [self, userId, made, pairing] {
            translated([&] {
                *made = e2ev3::startNewDevice(userId, self->backend());
                *pairing = e2ev3::NewDevicePairing::begin(**made);
            });
        },
        [this, made, pairing] {
            setBusy(false);
            device_ = *made;
            joiningPairing_ = *pairing;
            joining_ = true;
            emit changed();
            auto device = device_;
            auto pair = joiningPairing_;
            auto stop = stopWaiting_;
            runInBackground(
                waitPool_, this,
                [device, pair, stop] {
                    return translated([&] {
                        std::vector<int> codes = pair->codes(waitFor(stop));
                        return codes;
                    });
                },
                [this](std::vector<int> codes) {
                    joinCodes_.clear();
                    for (int c : codes) joinCodes_.append(c);
                    emit changed();
                },
                [this](const Failure& failure) { abandonJoin(failure.message); });
        },
        [this](const Failure& failure) {
            setBusy(false);
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            setError(failure.message);
            refresh();
        });
}

void DeviceKeysSession::abandonJoin(const QString& message) {
    auto device = device_;
    auto pair = joiningPairing_;
    joiningPairing_.reset();
    joining_ = false;
    joinCodes_.clear();
    if (!message.isEmpty()) setError(message);
    emit changed();
    // Whatever was half made goes: the keys it published, its enrollment request.
    runInBackground(
        pool_, this,
        [device, pair] {
            if (pair) pair->cancel();
            if (device) device->forget();
        },
        [this] {
            device_.reset();
            refresh();
        },
        [this](const Failure&) {
            device_.reset();
            refresh();
        });
}

void DeviceKeysSession::confirmJoin() {
    if (!joining_ || joinCodes_.isEmpty() || busy_) return;
    auto device = device_;
    auto pair = joiningPairing_;
    auto stop = stopWaiting_;
    setBusy(true);
    runInBackground(
        waitPool_, this, [device, pair, stop] { translated([&] { pair->confirm(waitFor(stop)); }); },
        [this] {
            setBusy(false);
            joining_ = false;
            joinCodes_.clear();
            joiningPairing_.reset();
            refresh();
        },
        [this](const Failure& failure) {
            setBusy(false);
            abandonJoin(failure.message);
        });
}

void DeviceKeysSession::cancelJoin() {
    if (!joining_) return;
    stopWaiting_->store(true);
    abandonJoin({});
}

void DeviceKeysSession::compareCodes(const QString& newDeviceId) {
    if (!device_ || busy_ || !answeringDeviceId_.isEmpty()) return;
    QString enrollmentId;
    for (const QVariant& w : waiting_) {
        const QVariantMap m = w.toMap();
        if (m.value("deviceId").toString() == newDeviceId) enrollmentId = m.value("enrollmentId").toString();
    }
    if (enrollmentId.isEmpty()) return;
    auto device = device_;
    stopWaiting_->store(false);
    auto stop = stopWaiting_;
    auto started = std::make_shared<e2ev3::TrustedPairing::Started>();
    setBusy(true);
    answeringDeviceId_ = newDeviceId;
    emit changed();
    runInBackground(
        waitPool_, this,
        [device, stop, enrollmentId, started] {
            translated([&] { *started = e2ev3::TrustedPairing::begin(*device, enrollmentId.toStdString(), waitFor(stop)); });
        },
        [this, started] {
            setBusy(false);
            answeringPairing_ = std::move(started->pairing);
            answerCodes_.clear();
            for (int c : started->codes) answerCodes_.append(c);
            emit changed();
        },
        [this](const Failure& failure) {
            setBusy(false);
            answeringDeviceId_.clear();
            setError(failure.message);
            refresh();
        });
}

void DeviceKeysSession::approve() {
    if (!answeringPairing_ || busy_) return;
    auto device = device_;
    auto pair = answeringPairing_;
    auto stop = stopWaiting_;
    setBusy(true);
    runInBackground(
        waitPool_, this, [device, pair, stop] { translated([&] { pair->confirm(waitFor(stop)); }); },
        [this] {
            setBusy(false);
            finishAnswer();
        },
        [this](const Failure& failure) {
            setBusy(false);
            finishAnswer();
            setError(failure.message);
        });
}

void DeviceKeysSession::finishAnswer() {
    answeringPairing_.reset();
    answeringDeviceId_.clear();
    answerCodes_.clear();
    emit changed();
    refresh();
}

void DeviceKeysSession::rejectAnswer() {
    if (answeringDeviceId_.isEmpty()) return;
    stopWaiting_->store(true);
    auto pair = answeringPairing_;
    if (pair) {
        runInBackground(
            pool_, this, [pair] { pair->reject(); }, [this] { finishAnswer(); }, [this](const Failure&) { finishAnswer(); });
    } else {
        finishAnswer();
    }
}

void DeviceKeysSession::turnOffSignal(const QString& deviceId) {
    // A signed change to the device list: only a device holding the account key can make it.
    auto device = device_;  // taken here, on the UI thread: device_ is only ever touched there
    if (!device) return;
    run("turnOffSignal", [device, id = deviceId.toStdString()] { e2ev3::turnOffSignal(*device, id); }, [] {});
}

void DeviceKeysSession::revoke(const QString& deviceId) {
    auto device = device_;
    if (!device) return;
    run("revoke", [device, id = deviceId.toStdString()] { e2ev3::revokeDevice(*device, id); });
}

void DeviceKeysSession::removeWaiting(const QString& deviceId) {
    auto server = server_;
    run("removeWaiting", [server, id = deviceId.toStdString()] { server->removeDevice(id); });
}

void DeviceKeysSession::createRecoveryKey() {
    auto device = device_;
    if (!device) return;
    auto key = std::make_shared<std::string>();
    run(
        "recovery", [device, key] { *key = e2ev3::createRecoveryBackup(*device); },
        [this, key] {
            recoveryKey_ = QString::fromStdString(*key);
            recoveryNotice_ = false;
            emit changed();
        });
}

void DeviceKeysSession::dismissRecoveryKey() {
    recoveryKey_.clear();
    emit changed();
}

void DeviceKeysSession::restore(const QString& recoveryKey) {
    if (device_) return;
    const std::string userId = identity_->userId;
    const std::string text = recoveryKey.toStdString();
    auto self = this;
    auto made = std::make_shared<std::shared_ptr<e2ev3::V3Device>>();
    run(
        "restore", [self, userId, text, made] { *made = e2ev3::restoreWithRecoveryKey(userId, text, self->backend()); },
        [this, made] { device_ = *made; });
}

void DeviceKeysSession::cancelReset() {
    auto device = device_;
    if (!device || pendingReset_.isEmpty()) return;
    run("cancelReset", [device] {
        const Json view = device->server().getDeviceList(device->userId());
        const Json pending = view.value("pendingReset", Json(nullptr));
        if (!pending.is_object()) return;
        const Json refusal = device->must("cancelReset", Json{{"pendingAccountKey", pending.at("accountKey")}});
        device->server().cancelReset(refusal.at("signature").get<std::string>());
    });
}

void DeviceKeysSession::forgetDevice() {
    auto device = device_;
    if (!device) return;
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const std::filesystem::path file = std::filesystem::path(root.toStdString()) / "device-keys" / ("chat-" + device->deviceId());
    run("forget", [device] { device->forget(); }, [this, file] {
        // The device-key history goes with the keys.
        std::error_code ec;
        std::filesystem::remove(file, ec);
        history_.reset();
        closeChat();
        device_.reset();
    });
}

void DeviceKeysSession::startOver() {
    auto device = device_;
    if (!device || joining_) return;
    run(
        "startOver",
        [device] {
            device->op("cancelLink");
            device->forget();
        },
        [this] { device_.reset(); });
}

void DeviceKeysSession::loadSafetyNumber(const QString& contactUserId, const QString& conversationId) {
    updateSafetyNumber(contactUserId, conversationId, {});
}

void DeviceKeysSession::setDeviceKeysVerified(const QString& contactUserId, const QString& conversationId, const QString& accountKey,
                                              bool verified) {
    updateSafetyNumber(contactUserId, conversationId,
                       [contact = contactUserId.toStdString(), key = accountKey.toStdString(), verified](e2ev3::V3Device& device) {
                           e2ev3::setContactVerified(device, contact, key, verified);
                       });
}

void DeviceKeysSession::acceptNewDeviceKeys(const QString& contactUserId, const QString& conversationId, const QString& accountKey,
                                            bool verified) {
    updateSafetyNumber(contactUserId, conversationId,
                       [contact = contactUserId.toStdString(), conversation = conversationId.toStdString(), key = accountKey.toStdString(),
                        verified](e2ev3::V3Device& device) { e2ev3::acceptContactKey(device, contact, key, verified, conversation); });
}

// Runs `change` (if any), then reads the number again, off the UI thread; answers with safetyNumberReady.
void DeviceKeysSession::updateSafetyNumber(const QString& contactUserId, const QString& conversationId,
                                           std::function<void(e2ev3::V3Device&)> change) {
    auto device = device_;
    if (!device || status_ != QStringLiteral("trusted")) {
        emit safetyNumberReady(contactUserId, {});
        return;
    }
    auto out = std::make_shared<QVariantMap>();
    runInBackground(
        pool_, this,
        [device, out, change = std::move(change), contact = contactUserId.toStdString(), conversation = conversationId.toStdString()] {
            try {
                if (change) change(*device);
                if (const auto safety = e2ev3::deviceKeySafety(*device, contact, conversation)) {
                    QStringList groups;
                    for (const auto& group : safety->groups) groups.append(qs(group));
                    (*out)[QStringLiteral("groups")] = groups;
                    (*out)[QStringLiteral("qrText")] = qs(safety->qrText);
                    (*out)[QStringLiteral("accountKey")] = qs(safety->accountKey);
                    (*out)[QStringLiteral("verified")] = safety->verified;
                    (*out)[QStringLiteral("keyChanged")] = safety->keyChanged;
                }
            } catch (const e2ev3::EngineError& error) {
                (*out)[QStringLiteral("problem")] = error.code() == "account_key_changed"
                                                        ? QStringLiteral("Their keys changed again while this was open. Close this and open it again.")
                                                        : QStringLiteral("Couldn't check their device keys. Try again.");
            } catch (const std::exception&) {
                (*out)[QStringLiteral("problem")] = QStringLiteral("Couldn't check their device keys. Try again.");
            }
        },
        [this, out, contactUserId] { emit safetyNumberReady(contactUserId, *out); },
        [this, contactUserId](const Failure&) {
            emit safetyNumberReady(contactUserId, QVariantMap{{QStringLiteral("problem"), QStringLiteral("Couldn't reach the server.")}});
        });
}

// --- Chat (docs/multi-device-threat-model.md section 9) ---

std::shared_ptr<e2ev3::ChatHistory> DeviceKeysSession::history() {
    if (!device_) return nullptr;
    if (history_ && historyDevice_ != device_->deviceId()) history_.reset();
    if (!history_) {
        historyDevice_ = device_->deviceId();
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const std::filesystem::path file = std::filesystem::path(root.toStdString()) / "device-keys" / ("chat-" + device_->deviceId());
        history_ = std::make_shared<e2ev3::ChatHistory>(file, device_->deriveKey("redvault-v3-chat-history"));
        // What the engine already encrypted while Tor was down, sealed under a key of its own (docs/tor-by-default.md).
        const std::filesystem::path queue = std::filesystem::path(root.toStdString()) / "device-keys" / ("sendq-" + device_->deviceId());
        queueing_->attach(std::make_shared<e2ev3::SendQueue>(queue, device_->deriveKey("redvault-v3-send-queue")));
    }
    if (!storyPart_->store || storyPart_->device != device_) {
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        // Part D: story keys, named answers and story messages waiting to go, sealed under this device's key.
        auto store = std::make_shared<e2ev3::StoryStore>(std::filesystem::path(root.toStdString()) / "device-keys" / ("stories-" + device_->deviceId()),
                                                         device_->deriveKey("redvault-v3-story-keys"));
        {
            std::lock_guard lock(storyPart_->mutex);
            storyPart_->device = device_;
            storyPart_->store = store;
        }
        std::lock_guard lock(callInbox_->mutex);
        callInbox_->stories = store;
    }
    if (communitiesDevice_ != device_->deviceId()) {
        communitiesDevice_ = device_->deviceId();
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        // Membership-task cursors and channel audiences: account ids only, sealed under this device's key.
        communities_->attachFile(std::filesystem::path(root.toStdString()) / "device-keys" / ("communities-" + device_->deviceId()),
                                 device_->deriveKey("redvault-v3-communities"));
    }
    return history_;
}

void DeviceKeysSession::setTor(TorController* tor) {
    tor_ = tor;
    if (tor_ != nullptr && tor_->enabled()) connect(tor_, &TorController::becameConnected, this, &DeviceKeysSession::drainQueued);
}

bool DeviceKeysSession::torDown() const {
    return tor_ != nullptr && tor_->enabled() && !tor_->connected();
}

void DeviceKeysSession::drainQueued() {
    auto device = device_;
    if (!device || draining_ || torDown() || !trusted()) return;
    auto h = history();
    auto stories = storyStore();
    const bool storiesWait = stories && stories->hasDeliveries();
    if (!h || (!e2ev3::hasQueuedChat(*h, *queueing_) && !storiesWait)) return;
    draining_ = true;
    auto server = queueing_;
    auto drained = std::make_shared<e2ev3::QueueDrain>();
    const auto fanOut = storyFanOut();
    runInBackground(
        chatPool_, this,
        [device, h, server, drained, stories, fanOut] {
            translated([&] {
                *drained = e2ev3::sendQueuedChat(*device, *h, *server);
                // Part D: story keys and answers that waited, after what the engine had already encrypted (in order).
                if (!drained->waiting && stories && stories->hasDeliveries()) e2ev3::sendQueuedStories(*device, *stories, fanOut);
            });
        },
        [this, drained] {
            draining_ = false;
            if (drained->failed > 0 && !chatId_.isEmpty()) {
                chatError_ = drained->failed == 1 ? QStringLiteral("A message that waited for Tor couldn't be sent. Retry it, or send it again.")
                                                  : QStringLiteral("%1 messages that waited for Tor couldn't be sent. Retry them, or send them again.")
                                                        .arg(drained->failed);
            }
            reloadChat();
            emit chatChanged();
        },
        [this](const Failure& failure) {
            draining_ = false;
            if (failure.status == 401) emit sessionExpired();
            reloadChat();
            emit chatChanged();
        });
}

void DeviceKeysSession::reloadChat() {
    QVariantList rows;
    QVariantList pins;
    readable_.clear();
    if (const auto h = history(); h && !chatId_.isEmpty()) {
        const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
        const std::string chat = chatId_.toStdString();
        const auto records = h->records(chat);
        const auto shown = e2ev3::foldChat(records, now);
        // What this account already said it read (here or on another of its devices).
        std::set<std::string> read;
        for (const e2ev3::ChatRecord& r : records) {
            if (!r.op || r.op->t != "receipt" || r.from != identity_->userId || r.op->status != "read") continue;
            read.insert(r.op->targets.begin(), r.op->targets.end());
        }
        std::map<std::string, const e2ev3::ChatRecord*> byId;
        for (const e2ev3::ChatRecord& r : shown) byId.emplace(r.id, &r);
        const ChatInfo info = openInfo();
        RowContext context{nullptr, &byId, &info, identity_->userId, now};
        for (const e2ev3::ChatRecord& r : shown) {
            rows.append(chatRow(r, context));
            context.previous = &r;
            if (r.pinned) pins.prepend(QVariantMap{{"messageId", qs(r.id)}, {"preview", qs(previewFor(r))}});
            // Someone else's message not yet read here: a read receipt once it's on screen.
            if (r.direction == "in" && !r.deleted && !read.count(r.id)) readable_[qs(r.id)] = r.from;
        }
    }
    chatPins_ = pins;
    chatTopNotice_.clear();
    if (const auto h = history(); h && chatCommunity_ && !chatId_.isEmpty()) chatTopNotice_ = qs(e2ev3::communityNotice(*h, chatId_.toStdString()));
    chatModel_.setRows(rows);
    scheduleExpiry();
}

bool DeviceKeysSession::chatPinned(const QString& chatId) {
    // Part C2: a community is on device keys only; nothing goes out on the older protocol there.
    if (!chatId.isEmpty() && communities_->find(chatId.toStdString())) return true;
    const auto h = history();
    if (h && h->pinned(chatId.toStdString())) return true;
    // The open chat on device keys, or not decided yet: never the older protocol meanwhile.
    return !chatId.isEmpty() && chatId == chatId_ &&
           (chatMode_ == QStringLiteral("device") || chatMode_ == QStringLiteral("checking") || chatMode_ == QStringLiteral("link"));
}

void DeviceKeysSession::openChat(const QString& chatId, const QString& contactUserId, bool isGroup, bool eligible) {
    chatId_ = chatId;
    chatContactUser_ = contactUserId;
    chatContact_ = QStringLiteral("unknown");
    contactCheckFor_.clear();
    chatProtocol_.clear();
    chatNotice_.clear();
    unread_.remove(chatId);
    chatIsGroup_ = isGroup;
    // Part C2: communities and their channels are full Signal groups on device keys.
    chatCommunity_ = communities_->find(chatId.toStdString()).has_value();
    chatTopNotice_.clear();
    chatEligible_ = (eligible || chatCommunity_) && available();
    chatError_.clear();
    chatMode_.clear();
    chatActive_ = false;
    reloadChat();
    emit chatChanged();
    decideChat();
}

void DeviceKeysSession::decideChat() {
    QString mode;
    if (chatId_.isEmpty() || !chatEligible_) {
        mode.clear();
    } else if (!known_) {
        // The server hasn't answered yet (or couldn't be reached): nothing goes out on the older protocol meanwhile.
        mode = QStringLiteral("checking");
    } else if (status_ == QStringLiteral("off")) {
        mode.clear();
    } else if (chatCommunity_) {
        // A community never uses the older protocol: device keys, or nothing until this computer is linked.
        mode = trusted() ? QStringLiteral("device") : QStringLiteral("link");
        if (const auto h = history(); h && trusted()) {
            const std::string chat = chatId_.toStdString();
            h->pin(chat);
            // From now on this device gets the community's messages; nothing older is on the server for it.
            e2ev3::noteCommunityJoined(*h, chat);
            chatTopNotice_ = qs(e2ev3::communityNotice(*h, chat));
        }
    } else if (status_ == QStringLiteral("setup")) {
        // This account has no device list yet (it's being set up): not every member has one.
        mode = QStringLiteral("older");
    } else {
        const auto h = history();
        const bool pinned = h && h->pinned(chatId_.toStdString());
        if (pinned) {
            mode = trusted() ? QStringLiteral("device") : QStringLiteral("link");
        } else if (chatContact_ == QStringLiteral("unknown")) {
            mode = QStringLiteral("checking");
        } else if (chatContact_ == QStringLiteral("yes")) {
            // Every other member has a signed list: device keys, which this computer can send with only once linked.
            mode = trusted() ? QStringLiteral("device") : QStringLiteral("link");
        } else {
            mode = QStringLiteral("older");
        }
        if (chatContact_ == QStringLiteral("unknown")) checkContact();
    }
    setChatMode(mode);
}

void DeviceKeysSession::checkContact() {
    // Does every other member have device keys? Their signed lists exist. Asked of the server directly, so it
    // works on a computer that isn't linked yet too; the protocol needs this computer's device.
    if (chatId_.isEmpty() || contactCheckFor_ == chatId_) return;
    contactCheckFor_ = chatId_;
    auto device = trusted() ? device_ : nullptr;
    auto server = server_;
    const std::string self = identity_->userId;
    const std::string chat = chatId_.toStdString();
    const std::string contact = chatContactUser_.toStdString();
    const bool group = chatIsGroup_;
    auto answer = std::make_shared<std::optional<bool>>();
    auto protocol = std::make_shared<std::string>();
    const QString forChat = chatId_;
    runInBackground(
        chatPool_, this,
        [device, server, self, chat, contact, group, answer, protocol] {
            try {
                if (!group && device) *protocol = e2ev3::chatProtocol(*device, chat, contact);
            } catch (const std::exception&) {
            }
            try {
                if (group) {
                    bool every = true;
                    int others = 0;
                    for (const Json& m : server->conversationDeviceLists(chat).value("members", Json::array())) {
                        if (m.value("userId", std::string()) == self) continue;
                        ++others;
                        every = every && m.contains("deviceList") && m.at("deviceList").is_object();
                    }
                    *answer = every && others > 0;
                } else if (!contact.empty()) {
                    const Json view = server->getDeviceList(contact, chat);
                    *answer = view.contains("deviceList") && view.at("deviceList").is_object();
                } else {
                    *answer = false;  // nobody on the other side to have keys
                }
            } catch (const std::exception&) {
                // Not known: asked again shortly.
            }
        },
        [this, answer, protocol, forChat] {
            if (forChat != chatId_) return;
            contactCheckFor_.clear();
            chatProtocol_ = qs(*protocol);
            if (!answer->has_value()) {
                // Couldn't ask: the chat waits (nothing goes out on the older protocol meanwhile) and asks again.
                QTimer::singleShot(10000, this, [this, forChat] {
                    if (forChat == chatId_ && chatContact_ == QStringLiteral("unknown")) decideChat();
                });
                emit chatChanged();
                return;
            }
            chatContact_ = **answer ? QStringLiteral("yes") : QStringLiteral("no");
            emit chatChanged();
            decideChat();
        },
        [this, forChat](const Failure&) {
            if (forChat == chatId_) contactCheckFor_.clear();
        });
}

void DeviceKeysSession::closeChat() {
    if (voicePlayer_) voicePlayer_->stop();
    voiceId_.clear();
    readable_.clear();
    chatPins_.clear();
    chatId_.clear();
    chatActive_ = false;
    chatMode_.clear();
    chatContact_ = QStringLiteral("unknown");
    contactCheckFor_.clear();
    chatModel_.setRows({});
    receiveTimer_.setInterval(30000);
    emit chatChanged();
}

void DeviceKeysSession::useOlderProtocol() {
    if (const auto h = history(); h && !chatId_.isEmpty()) h->unpin(chatId_.toStdString());
    chatError_.clear();
    setChatMode(QStringLiteral("older"));
}

void DeviceKeysSession::clearChatError() {
    if (chatError_.isEmpty()) return;
    chatError_.clear();
    emit chatChanged();
}

void DeviceKeysSession::chatFailed(const Failure& failure, const QString& fallback) {
    if (failure.status == 401) {
        emit sessionExpired();
        return;
    }
    chatError_ = failure.message.isEmpty() ? fallback : failure.message;
}

void DeviceKeysSession::setChatFileActivity(const QString& activity) {
    if (chatFileActivity_ == activity) return;
    chatFileActivity_ = activity;
    emit chatChanged();
}

void DeviceKeysSession::runChat(ChatWork work, const QString& fallback, bool fromComposer, ChatWork queue,
                                std::function<void(const QString& error)> finished) {
    auto device = device_;
    auto h = history();
    if (!device || !h) {
        if (finished) finished(fallback);
        return;
    }
    if (fromComposer) chatSending_ = true;
    chatError_.clear();
    chatNotice_.clear();
    lastUnreachable_->store(0);
    emit chatChanged();
    // Tor isn't connected: the send waits, encrypted on this computer, and nothing is tried (docs/tor-by-default.md).
    const bool offline = torDown();
    auto waited = std::make_shared<std::atomic<bool>>(false);
    runInBackground(
        chatPool_, this,
        [device, h, inbox = callInbox_, server = queueing_, offline, waited, chat = chatId_.toStdString(), contact = chatTarget(), work = std::move(work),
         queue = std::move(queue)] {
            translated([&] {
                // Earlier sends still waiting go first: this one queues behind them, so nothing overtakes.
                if (queue && (offline || e2ev3::hasQueuedChat(*h, *server))) {
                    queue(*device, *h, chat, contact);
                    waited->store(true);
                    return;
                }
                // Anything that arrived first is read before sending (9.6).
                receiveWithCalls(*device, *h, inbox.get());
                work(*device, *h, chat, contact);
                // Tor went down on the way: the send is queued (send_queue.hpp), never lost.
                if (!h->queued().empty()) waited->store(true);
            });
        },
        [this, fromComposer, waited, finished] {
            if (fromComposer) chatSending_ = false;
            const int unreachable = lastUnreachable_->load();
            if (unreachable > 0) {
                chatNotice_ = unreachable == 1 ? QStringLiteral("Sent, but one of their devices couldn't be reached. It won't get this message.")
                                               : QStringLiteral("Sent, but %1 of their devices couldn't be reached. They won't get this message.").arg(unreachable);
            }
            if (waited->load()) {
                chatNotice_ = QStringLiteral("Waiting for Tor. It's kept encrypted on this computer and sent once Tor connects.");
                drainQueued();  // only if Tor came back meanwhile
            }
            reloadChat();
            checkContact();  // the protocol may have changed with the lists this send verified
            emit chatChanged();
            if (fromComposer) emit chatSent(true);
            if (finished) finished({});
        },
        [this, fromComposer, fallback, finished](const Failure& failure) {
            if (fromComposer) chatSending_ = false;
            chatFailed(failure, fallback);
            reloadChat();  // a failed message is listed, with Retry
            emit chatChanged();
            if (fromComposer) emit chatSent(false);
            if (finished) finished(chatError_.isEmpty() ? fallback : chatError_);
        });
}

void DeviceKeysSession::sendMessage(e2ev3::OutgoingMessage message, const QString& fallback, bool fromComposer) {
    runChat(
        [message, unreachable = lastUnreachable_](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            unreachable->store(e2ev3::sendChatMessage(device, h, chat, contact, message).unreachable);
        },
        fallback, fromComposer,
        [message](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            e2ev3::queueChatMessage(device, h, chat, contact, message);
        });
}

void DeviceKeysSession::sendChat(const QString& text, const QString& replyToId) {
    if (!chatActive_ || chatSending_ || text.trimmed().isEmpty()) return;
    e2ev3::OutgoingMessage message;
    message.text = text.trimmed().toStdString();
    // Your timer in this chat: each message carries it (part A), as in protocol-1 chats.
    message.extras.exp = chatTimer();
    if (!replyToId.isEmpty()) {
        if (const auto target = shownMessage(replyToId); target && !target->deleted) {
            message.extras.replyTo = target->id;
            message.extras.replyPreview = e2ev3::previewText(previewFor(*target));
        }
    }
    sendMessage(std::move(message), QStringLiteral("The message wasn't sent."), true);
}

void DeviceKeysSession::retryChat(const QString& messageId) {
    const auto h = history();
    if (!h || chatSending_) return;
    const auto failed = shownMessage(messageId);
    if (!failed || failed->status != "failed") return;
    const std::string chat = chatId_.toStdString();
    // Sent again under a fresh id and ciphertext, never reused; what it said (a reply, a poll, its timer) stays.
    e2ev3::OutgoingMessage message;
    message.text = failed->text;
    message.file = failed->file;
    if (failed->poll) message.poll = e2ev3::ChatPoll{failed->poll->options, failed->poll->multi, {}, false, {}};
    message.extras = e2ev3::MessageExtras{failed->replyTo, failed->replyPreview, failed->forwarded, failed->exp, failed->storyReply};
    h->remove(chat, failed->id);
    reloadChat();
    sendMessage(std::move(message), failed->file ? QStringLiteral("The file wasn't sent.") : QStringLiteral("The message wasn't sent."), true);
}

void DeviceKeysSession::discardChat(const QString& messageId) {
    const auto h = history();
    const auto failed = shownMessage(messageId);
    if (!h || !failed || failed->status != "failed") return;  // a queued one may already be encrypted: it goes as it is
    h->remove(chatId_.toStdString(), failed->id);
    reloadChat();
    emit chatChanged();
}

QString DeviceKeysSession::previewOf(const QString& messageId) {
    const auto message = shownMessage(messageId);
    return message ? qs(previewFor(*message)) : QString();
}

void DeviceKeysSession::createPoll(const QString& question, const QStringList& options, bool multiple) {
    if (!chatActive_) {
        emit pollDone(QStringLiteral("This chat can't take a poll right now."));
        return;
    }
    e2ev3::OutgoingMessage message;
    message.text = question.trimmed().toStdString();
    e2ev3::ChatPoll poll;
    for (const QString& option : options) poll.options.push_back(option.trimmed().toStdString());
    poll.multi = multiple;
    try {
        (void)e2ev3::encodePoll(chatId_.toStdString(), message.text, poll.options, poll.multi, util::randomUuid(), 1);
    } catch (const e2ev3::EngineError&) {
        emit pollDone(QStringLiteral("Write a question (up to 300 characters) and 2 to 10 different options (up to 100 characters each)."));
        return;
    }
    message.poll = std::move(poll);
    message.extras.exp = chatTimer();
    runChat(
        [message](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            e2ev3::sendChatMessage(device, h, chat, contact, message);
        },
        QStringLiteral("The poll wasn't sent."), false,
        [message](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            e2ev3::queueChatMessage(device, h, chat, contact, message);
        },
        [this](const QString& error) { emit pollDone(error); });
}

void DeviceKeysSession::vote(const QString& pollId, const QVariantList& options) {
    const auto poll = shownMessage(pollId);
    if (!chatActive_ || !poll || !poll->poll || poll->poll->ended || poll->deleted || poll->status == "failed" || poll->status == "queued") return;
    e2ev3::ChatOp op;
    op.t = "vote";
    op.target = poll->id;
    for (const QVariant& option : options) {
        const int index = option.toInt();
        if (index < 0 || static_cast<std::size_t>(index) >= poll->poll->options.size()) return;
        if (std::find(op.choices.begin(), op.choices.end(), index) == op.choices.end()) op.choices.push_back(index);
    }
    if (!poll->poll->multi && op.choices.size() > 1) return;
    runChat([op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) { e2ev3::sendChatOp(device, h, chat, contact, op); },
            QStringLiteral("Your vote didn't go through. Check your connection and try again."), false,
            [op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) { e2ev3::queueChatOp(device, h, chat, contact, op); });
}

void DeviceKeysSession::endPoll(const QString& pollId) {
    const auto poll = shownMessage(pollId);
    if (!chatActive_ || !poll || !poll->poll || poll->poll->ended || poll->direction != "out" || poll->deleted) return;
    const e2ev3::ChatOp op{"end", poll->id};
    runChat([op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) { e2ev3::sendChatOp(device, h, chat, contact, op); },
            QStringLiteral("That didn't go through. Check your connection and try again."), false,
            [op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) { e2ev3::queueChatOp(device, h, chat, contact, op); });
}

void DeviceKeysSession::pinChat(const QString& messageId, bool pinned) {
    const auto message = shownMessage(messageId);
    if (!chatActive_ || !message || message->deleted || message->status == "failed" || message->status == "queued") return;
    if (message->pinned == pinned) return;
    e2ev3::ChatOp op{"pin", message->id};
    op.pinned = pinned;
    runChat([op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) { e2ev3::sendChatOp(device, h, chat, contact, op); },
            QStringLiteral("That didn't go through. Check your connection and try again."), false,
            [op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) { e2ev3::queueChatOp(device, h, chat, contact, op); });
}

QStringList DeviceKeysSession::forwardable(const QStringList& messageIds) {
    QStringList out;
    for (const QString& id : messageIds) {
        if (forwardItem(id)) out.append(id);
    }
    return out;
}

bool DeviceKeysSession::anyFile(const QStringList& messageIds) {
    for (const QString& id : messageIds) {
        if (const auto item = forwardItem(id); item && item->file) return true;
    }
    return false;
}

bool DeviceKeysSession::allFromMe(const QStringList& messageIds) {
    if (messageIds.isEmpty()) return false;
    for (const QString& id : messageIds) {
        const auto message = shownMessage(id);
        if (!message || message->direction != "out" || message->deleted || message->status == "failed" || message->status == "queued") return false;
    }
    return true;
}

void DeviceKeysSession::deleteMany(const QStringList& messageIds) {
    for (const QString& id : messageIds) deleteChat(id);
}

std::optional<DeviceKeysSession::ForwardItem> DeviceKeysSession::forwardItem(const QString& messageId) {
    const auto message = shownMessage(messageId);
    // Something that reached the chat, and isn't a poll (a poll's votes belong to its own chat).
    if (!message || message->deleted || message->poll || message->status == "failed" || message->status == "queued") return std::nullopt;
    if (!message->file && message->text.empty()) return std::nullopt;
    return ForwardItem{message->file ? std::string() : message->text, message->file, message->ts};
}

void DeviceKeysSession::forwardInto(const QString& chatId, std::vector<ForwardItem> items) {
    auto device = device_;
    auto h = history();
    if (!device || !h || !trusted() || items.empty() || !chatInfo_) return;
    const ChatInfo info = chatInfo_(chatId.toStdString());
    if (!info.known || (!info.isGroup && info.contactUserId.empty())) return;
    const std::string chat = chatId.toStdString();
    const std::string contact = info.isGroup ? std::string() : info.contactUserId;
    const int exp = info.timerSeconds;  // the target chat's own timer
    const bool offline = torDown();
    if (offline) {
        // A file has to be uploaded again first, which needs Tor; texts wait in the queue like any send.
        const auto before = items.size();
        std::erase_if(items, [](const ForwardItem& item) { return item.file.has_value(); });
        if (items.size() != before) {
            chatError_ = QStringLiteral("Can't reach Tor, so the files weren't forwarded. Forward them again once Tor is connected.");
            emit chatChanged();
        }
        if (items.empty()) return;
    }
    // Each file: decrypted here, encrypted with a new key and uploaded for that chat (off the chat thread: it can be large).
    runInBackground(
        waitPool_, this,
        [files = files_, items, chat] {
            std::vector<std::optional<model::FileDescriptor>> fresh(items.size());
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (!items[i].file) continue;
                const auto plain = files->download(*items[i].file);
                try {
                    fresh[i] = files->uploadForDeviceKeys(chat, plain);
                } catch (...) {
                    std::error_code ignored;
                    std::filesystem::remove_all(plain.parent_path(), ignored);
                    throw;
                }
                std::error_code ignored;
                std::filesystem::remove_all(plain.parent_path(), ignored);
            }
            return fresh;
        },
        [this, device, h, chat, contact, exp, items](std::vector<std::optional<model::FileDescriptor>> fresh) {
            runInBackground(
                chatPool_, this,
                [device, h, inbox = callInbox_, server = queueing_, offline = torDown(), chat, contact, exp, items, fresh] {
                    translated([&] {
                        const bool wait = offline || e2ev3::hasQueuedChat(*h, *server);
                        if (!wait) receiveWithCalls(*device, *h, inbox.get());
                        for (std::size_t i = 0; i < items.size(); ++i) {
                            if (items[i].file) {
                                if (!fresh[i]) continue;
                                const auto upload = *fresh[i];
                                e2ev3::forwardChatFile(*device, *h, chat, contact, *items[i].file, [&upload](const model::FileDescriptor&) { return upload; },
                                                       exp, wait);
                                continue;
                            }
                            e2ev3::OutgoingMessage message;
                            message.text = items[i].text;
                            message.extras.forwarded = true;
                            message.extras.exp = exp;
                            if (wait) {
                                e2ev3::queueChatMessage(*device, *h, chat, contact, message);
                            } else {
                                e2ev3::sendChatMessage(*device, *h, chat, contact, message);
                            }
                        }
                    });
                },
                [this] {
                    reloadChat();
                    emit chatChanged();
                    drainQueued();
                },
                [this](const Failure& failure) {
                    chatFailed(failure, QStringLiteral("The forward didn't go through. Check your connection and try again."));
                    reloadChat();
                    emit chatChanged();
                });
        },
        [this](const Failure& failure) {
            chatFailed(failure, QStringLiteral("The file couldn't be forwarded. Try again."));
            emit chatChanged();
        });
}

// --- Receipts and disappearing messages (part A) ---

void DeviceKeysSession::markShown(const QString& messageId) {
    if (!chatActive_) return;
    const auto it = readable_.find(messageId);
    if (it == readable_.end()) return;
    const std::string author = it->second;
    readable_.erase(it);
    // "read" only when the person allows read receipts; "delivered" went when it was stored.
    if (!local_store::readReceipts()) return;
    receipts_->add(chatId_.toStdString(), author, messageId.toStdString(), "read");
    scheduleReceipts();
}

void DeviceKeysSession::scheduleReceipts() {
    const auto next = receipts_->nextDue();
    if (!next) {
        receiptTimer_.stop();
        return;
    }
    const std::int64_t wait = std::max<std::int64_t>(0, *next - QDateTime::currentMSecsSinceEpoch());
    if (!receiptTimer_.isActive() || receiptTimer_.remainingTime() > wait) receiptTimer_.start(static_cast<int>(std::min<std::int64_t>(wait, 60000)));
}

void DeviceKeysSession::flushReceipts() {
    auto device = device_;
    auto h = history();
    if (!device || !h || !trusted()) return;
    const auto batches = receipts_->due(QDateTime::currentMSecsSinceEpoch());
    if (batches.empty()) {
        scheduleReceipts();
        return;
    }
    auto failed = std::make_shared<std::vector<e2ev3::ReceiptBatcher::Batch>>();
    runInBackground(
        chatPool_, this,
        [device, h, server = queueing_, offline = torDown(), batches, failed, self = identity_->userId] {
            for (const auto& batch : batches) {
                std::vector<std::string> ids;
                for (const std::string& id : batch.ids) {
                    if (!h->receiptSent(batch.conversationId, id, self, batch.status)) ids.push_back(id);
                }
                if (ids.empty() || batch.authorUserId.empty() || batch.authorUserId == self) continue;
                try {
                    if (offline || e2ev3::hasQueuedChat(*h, *server)) {
                        e2ev3::queueReceipt(*device, *h, batch.conversationId, batch.authorUserId, ids, batch.status);
                    } else {
                        e2ev3::sendReceipt(*device, *h, batch.conversationId, batch.authorUserId, ids, batch.status);
                    }
                } catch (const std::exception&) {
                    // Never shown: it's tried again on the next pass.
                    failed->push_back(e2ev3::ReceiptBatcher::Batch{batch.conversationId, batch.authorUserId, batch.status, ids});
                }
            }
        },
        [this, failed] {
            // Tried again on the next mailbox pass (receiveNow), not in a tight loop.
            for (auto& batch : *failed) retryReceipts_.push_back(std::move(batch));
            scheduleReceipts();
            drainQueued();
        },
        [this, batches](const Failure&) {
            for (const auto& batch : batches) retryReceipts_.push_back(batch);
            scheduleReceipts();
        });
}

void DeviceKeysSession::scheduleExpiry() {
    const auto h = history();
    const auto next = h ? h->nextExpiry() : std::nullopt;
    if (!next) {
        expiryTimer_.stop();
        return;
    }
    // At most an hour at a time (a timer can't wait four weeks); a little after, so it's surely due.
    const std::int64_t wait = std::clamp<std::int64_t>(*next - QDateTime::currentMSecsSinceEpoch() + 50, 0, 3600000);
    expiryTimer_.start(static_cast<int>(wait));
}

void DeviceKeysSession::eraseExpiredNow() {
    auto h = history();
    if (!h) return;
    auto changed = std::make_shared<bool>(false);
    runInBackground(
        chatPool_, this, [h, changed] { *changed = h->eraseExpired(); },
        [this, changed] {
            reloadChat();  // also schedules the next one
            if (*changed) emit chatChanged();
        },
        [this](const Failure&) { scheduleExpiry(); });
}

// --- Pictures and voice messages: decrypted in memory only, like protocol-1 chats ---

QVariantMap DeviceKeysSession::imageInfo(const QString& messageId) const {
    const auto it = imageStates_.find(messageId);
    const QString state = it == imageStates_.end() ? QString() : it->second;
    const QSize size = ChatImageCache::instance().thumbSize(messageId);
    return {{"state", state}, {"width", size.width()}, {"height", size.height()}};
}

void DeviceKeysSession::loadImage(const QString& messageId, bool force) {
    constexpr std::int64_t kAutoLoadBytes = 20LL * 1024 * 1024;
    const auto message = shownMessage(messageId);
    if (!message || !message->file || !isImageMime(message->file->mimeType) || message->deleted) return;
    if (const auto it = imageStates_.find(messageId); it != imageStates_.end() && it->second != QStringLiteral("failed")) {
        if (it->second == QStringLiteral("ready") && ChatImageCache::instance().has(messageId)) return;
        if (it->second == QStringLiteral("loading") || it->second == QStringLiteral("large")) {
            if (!(force && it->second == QStringLiteral("large"))) return;
        }
    }
    if (!force && message->file->originalSize > kAutoLoadBytes) {
        imageStates_[messageId] = QStringLiteral("large");
        ++imagesRevision_;
        emit imagesChanged();
        return;
    }
    imageStates_[messageId] = QStringLiteral("loading");
    ++imagesRevision_;
    emit imagesChanged();
    struct Decoded {
        QImage thumb;
        QByteArray bytes;
        bool ok = false;
    };
    runInBackground(
        waitPool_, this,
        [files = files_, descriptor = *message->file] {
            Decoded out;
            const auto path = files->download(descriptor);
            QFile file(QString::fromStdWString(path.wstring()));
            if (file.open(QIODevice::ReadOnly)) out.bytes = file.readAll();
            file.close();
            // The decrypted copy goes straight away: pictures are kept in memory only.
            std::error_code ignored;
            std::filesystem::remove_all(path.parent_path(), ignored);
            if (const auto image = ChatImageCache::decode(out.bytes)) {
                out.thumb = ChatImageCache::thumbnail(*image);
                out.ok = true;
            }
            return out;
        },
        [this, messageId](Decoded decoded) {
            if (decoded.ok) ChatImageCache::instance().put(messageId, decoded.thumb, decoded.bytes);
            imageStates_[messageId] = decoded.ok ? QStringLiteral("ready") : QStringLiteral("failed");
            ++imagesRevision_;
            emit imagesChanged();
        },
        [this, messageId](const Failure& failure) {
            imageStates_[messageId] = QStringLiteral("failed");
            ++imagesRevision_;
            emit imagesChanged();
            if (failure.status == 401) emit sessionExpired();
        });
}

void DeviceKeysSession::toggleVoice(const QString& messageId) {
    if (voicePlayer_ && voiceId_ == messageId) {
        voicePlaying() ? voicePlayer_->pause() : voicePlayer_->play();
        return;
    }
    const auto file = shownFile(messageId);
    if (!file || !isAudioMime(file->mimeType)) return;
    if (!voicePlayer_) {
        voicePlayer_ = std::make_unique<QMediaPlayer>();
        voiceOutput_ = std::make_unique<QAudioOutput>();
        voicePlayer_->setAudioOutput(voiceOutput_.get());
        connect(voicePlayer_.get(), &QMediaPlayer::positionChanged, this, &DeviceKeysSession::voiceChanged);
        connect(voicePlayer_.get(), &QMediaPlayer::durationChanged, this, &DeviceKeysSession::voiceChanged);
        connect(voicePlayer_.get(), &QMediaPlayer::playbackStateChanged, this, &DeviceKeysSession::voiceChanged);
    }
    voicePlayer_->stop();
    voicePlayer_->setSourceDevice(nullptr);
    voiceBuffer_.reset();
    voiceId_ = messageId;
    emit voiceChanged();
    const QString name = qs(file->filename);
    runInBackground(
        waitPool_, this,
        [files = files_, descriptor = *file] {
            const auto path = files->download(descriptor);
            QFile in(QString::fromStdWString(path.wstring()));
            QByteArray bytes;
            if (in.open(QIODevice::ReadOnly)) bytes = in.readAll();
            in.close();
            // In memory only: the decrypted copy goes at once.
            std::error_code ignored;
            std::filesystem::remove_all(path.parent_path(), ignored);
            return bytes;
        },
        [this, messageId, name](QByteArray bytes) {
            if (voiceId_ != messageId || !voicePlayer_) return;  // another one was picked meanwhile
            voiceBuffer_ = std::make_unique<QBuffer>();
            voiceBuffer_->setData(bytes);
            voiceBuffer_->open(QIODevice::ReadOnly);
            voicePlayer_->setSourceDevice(voiceBuffer_.get(), QUrl(QStringLiteral("voice:") + name));
            voicePlayer_->play();
        },
        [this, messageId](const Failure& failure) {
            if (voiceId_ == messageId) voiceId_.clear();
            chatFailed(failure, QStringLiteral("That voice message couldn't be played."));
            emit voiceChanged();
            emit chatChanged();
        });
}

std::optional<e2ev3::ChatRecord> DeviceKeysSession::shownMessage(const QString& messageId) {
    const auto h = history();
    if (!h || chatId_.isEmpty()) return std::nullopt;
    const std::string id = messageId.toStdString();
    for (e2ev3::ChatRecord& r : h->list(chatId_.toStdString())) {
        if (r.id == id) return std::move(r);
    }
    return std::nullopt;
}

std::optional<model::FileDescriptor> DeviceKeysSession::shownFile(const QString& messageId) {
    const auto message = shownMessage(messageId);
    return message ? message->file : std::nullopt;
}

void DeviceKeysSession::reactChat(const QString& messageId, const QString& emoji) {
    const auto message = shownMessage(messageId);
    if (!chatActive_ || !message || message->deleted || message->status == "failed") return;
    const std::string chosen = emoji.toStdString();
    bool already = false;
    for (const e2ev3::ChatReaction& r : message->reactions) {
        already = already || (r.userId == identity_->userId && r.emoji == chosen);
    }
    const e2ev3::ChatOp op{"reaction", message->id, {}, already ? std::string() : chosen};
    runChat(
        [op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            e2ev3::sendChatOp(device, h, chat, contact, op);
        },
        QStringLiteral("That didn't go through. Check your connection and try again."), false,
        [op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            e2ev3::queueChatOp(device, h, chat, contact, op);
        });
}

void DeviceKeysSession::editChat(const QString& messageId, const QString& text) {
    const auto message = shownMessage(messageId);
    const std::string body = text.trimmed().toStdString();
    if (!chatActive_ || chatSending_ || !message || body.empty()) return;
    if (message->direction != "out" || message->deleted || message->file || message->status == "failed") return;
    if (message->text == body) {  // nothing changed: nothing to send, and it doesn't read as edited
        emit chatSent(true);
        return;
    }
    const e2ev3::ChatOp op{"edit", message->id, body, {}};
    runChat(
        [op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            e2ev3::sendChatOp(device, h, chat, contact, op);
        },
        QStringLiteral("That didn't go through. Check your connection and try again."), true,
        [op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            e2ev3::queueChatOp(device, h, chat, contact, op);
        });
}

void DeviceKeysSession::deleteChat(const QString& messageId) {
    const auto message = shownMessage(messageId);
    if (!chatActive_ || !message || message->direction != "out" || message->deleted || message->status == "failed") return;
    const e2ev3::ChatOp op{"delete", message->id, {}, {}};
    const std::string objectId = message->file ? message->file->objectId : std::string();
    auto api = api_;
    runChat(
        [op, objectId, api](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            e2ev3::sendChatOp(device, h, chat, contact, op);
            if (objectId.empty()) return;
            try {
                api->deleteFile(objectId);  // best effort: without its key the file is unreadable anyway
            } catch (const std::exception&) {
            }
        },
        QStringLiteral("That didn't go through. Check your connection and try again."), false,
        // Waiting for Tor: the delete goes later; the encrypted file's server copy expires on its own.
        [op](e2ev3::V3Device& device, e2ev3::ChatHistory& h, const std::string& chat, const std::string& contact) {
            e2ev3::queueChatOp(device, h, chat, contact, op);
        });
}

void DeviceKeysSession::sendFile(const QUrl& file) {
    auto device = device_;
    auto h = history();
    if (!device || !h || !chatActive_ || !chatFileActivity_.isEmpty()) return;
    const std::string chat = chatId_.toStdString();
    const std::string contact = chatTarget();
    const std::filesystem::path path(file.toLocalFile().toStdWString());
    const QString fileUrl = file.toString();
    const QString fallback = QStringLiteral("The file wasn't sent.");
    if (torDown()) {
        // The encrypted upload itself needs Tor; nothing is sent any other way.
        chatError_ = QStringLiteral("Can't reach Tor, so the file wasn't sent. Send it again once Tor is connected.");
        emit chatChanged();
        emit chatFileDone(fileUrl);
        return;
    }
    chatError_.clear();
    setChatFileActivity(QStringLiteral("Encrypting…"));
    // Stage updates arrive on the worker thread; hop to the UI thread to show them.
    QPointer<DeviceKeysSession> self(this);
    auto onStage = [self](data::FileSendStage stage) {
        QMetaObject::invokeMethod(
            qApp,
            [self, stage] {
                if (!self || self->chatFileActivity_.isEmpty()) return;
                self->setChatFileActivity(stage == data::FileSendStage::Encrypting  ? QStringLiteral("Encrypting…")
                                          : stage == data::FileSendStage::Uploading ? QStringLiteral("Uploading…")
                                                                                    : QStringLiteral("Sending…"));
            },
            Qt::QueuedConnection);
    };
    auto finished = [this, fileUrl, fallback](const Failure* failure) {
        setChatFileActivity({});
        if (failure) chatFailed(*failure, fallback);
        reloadChat();
        emit chatChanged();
        emit chatFileDone(fileUrl);
    };
    // The transfer isn't on the chat thread: a large file mustn't stop messages arriving.
    runInBackground(
        waitPool_, this, [files = files_, chat, path, onStage] { return files->uploadForDeviceKeys(chat, path, onStage); },
        [this, device, h, chat, contact, finished](model::FileDescriptor descriptor) {
            runInBackground(
                chatPool_, this,
                [device, h, inbox = callInbox_, server = queueing_, offline = torDown(), chat, contact, descriptor = std::move(descriptor)] {
                    translated([&] {
                        // Uploaded, but Tor is down now or earlier sends wait: its message queues behind them.
                        if (offline || e2ev3::hasQueuedChat(*h, *server)) {
                            e2ev3::queueChatFile(*device, *h, chat, contact, descriptor);
                            return;
                        }
                        receiveWithCalls(*device, *h, inbox.get());
                        e2ev3::sendChatFile(*device, *h, chat, contact, descriptor);
                    });
                },
                [this, finished] {
                    finished(nullptr);
                    drainQueued();
                },
                [finished](const Failure& failure) { finished(&failure); });
        },
        [finished](const Failure& failure) { finished(&failure); });
}

void DeviceKeysSession::openFile(const QString& messageId) {
    const auto file = shownFile(messageId);
    if (!file || !chatFileActivity_.isEmpty()) return;
    if (!file_safety::isSafeToOpen(qs(file->filename))) {
        chatError_ = QStringLiteral("For your safety, only pictures, audio, video, text and PDF files open from a chat. Save this one instead.");
        emit chatChanged();
        return;
    }
    setChatFileActivity(QStringLiteral("Decrypting ") + qs(file->filename) + QStringLiteral("…"));
    runInBackground(
        waitPool_, this,
        [files = files_, descriptor = *file] {
            const auto path = files->download(descriptor);
            file_safety::markFromInternet(path);
            return path;
        },
        [this](std::filesystem::path path) {
            setChatFileActivity({});
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(path.wstring())))) {
                chatError_ = QStringLiteral("There's no app on this computer to open that kind of file. Save it instead.");
                emit chatChanged();
            }
        },
        [this](const Failure& failure) {
            setChatFileActivity({});
            chatFailed(failure, QStringLiteral("Download failed. Try again."));
            emit chatChanged();
        });
}

void DeviceKeysSession::saveFile(const QString& messageId, const QUrl& destination) {
    const auto file = shownFile(messageId);
    if (!file || !chatFileActivity_.isEmpty()) return;
    const std::filesystem::path target(destination.toLocalFile().toStdWString());
    setChatFileActivity(QStringLiteral("Decrypting ") + qs(file->filename) + QStringLiteral("…"));
    runInBackground(
        waitPool_, this,
        [files = files_, descriptor = *file, target] {
            const auto decrypted = files->download(descriptor);
            // The save dialog has already asked before replacing an existing file.
            std::filesystem::copy_file(decrypted, target, std::filesystem::copy_options::overwrite_existing);
            file_safety::markFromInternet(target);
            // The decrypted copy in the cache isn't needed once it's saved.
            std::error_code ignored;
            std::filesystem::remove_all(decrypted.parent_path(), ignored);
        },
        [this, target] {
            setChatFileActivity({});
            emit chatFileSaved(QDir::toNativeSeparators(QString::fromStdWString(target.wstring())));
        },
        [this](const Failure& failure) {
            setChatFileActivity({});
            chatFailed(failure, QStringLiteral("Download failed. Try again."));
            emit chatChanged();
        });
}

void DeviceKeysSession::receiveNow() {
    auto device = device_;
    auto h = history();
    if (!device || !h || !trusted()) return;
    if (chatReceiving_) {
        receiveAgain_ = true;  // a call's offer may have just arrived: read once more when this pass is done
        return;
    }
    drainQueued();  // what waited for Tor, if it's back (queued on the same chat thread, so before this receive)
    chatReceiving_ = true;
    auto fresh = std::make_shared<std::vector<e2ev3::ChatRecord>>();
    auto stats = std::make_shared<e2ev3::ReceiveStats>();
    runInBackground(
        chatPool_, this,
        [device, h, inbox = callInbox_, fresh, stats] { translated([&] { *fresh = receiveWithCalls(*device, *h, inbox.get(), stats.get()); }); },
        [this, fresh, stats] {
            chatReceiving_ = false;
            if (receiveAgain_) {
                receiveAgain_ = false;
                QTimer::singleShot(0, this, &DeviceKeysSession::receiveNow);
            }
            if (stats->heldForKeys != chatHeld_) {
                chatHeld_ = stats->heldForKeys;
                emit chatChanged();
            }
            int elsewhere = 0;
            bool reload = false;
            // Receipts that couldn't go last time: this pass tries them again.
            for (const auto& batch : retryReceipts_) {
                for (const std::string& id : batch.ids) receipts_->add(batch.conversationId, batch.authorUserId, id, batch.status);
            }
            retryReceipts_.clear();
            for (const auto& r : *fresh) {
                const QString chat = qs(r.conversationId);
                // Stored on this device: "delivered" to its author (part A). Never for an op (a receipt, vote, pin...).
                if (r.direction == "in" && !r.op && !r.from.empty()) receipts_->add(r.conversationId, r.from, r.id, "delivered");
                // Read on another of this account's devices: the unread count here follows.
                if (r.op && r.op->t == "receipt" && r.op->status == "read" && r.from == identity_->userId && chat != chatId_) {
                    const int left = std::max(0, unread_.value(chat).toInt() - static_cast<int>(r.op->targets.size()));
                    if (left == 0) {
                        unread_.remove(chat);
                    } else {
                        unread_[chat] = left;
                    }
                }
                if (chat == chatId_) {
                    reload = true;
                } else if (r.direction == "in" && !r.op) {  // a reaction, edit, vote, pin or receipt isn't a new message
                    ++elsewhere;
                    unread_[chat] = unread_.value(chat).toInt() + 1;
                }
            }
            if (reload) {
                reloadChat();
            } else if (!fresh->empty()) {
                scheduleExpiry();  // a disappearing message in another chat
            }
            scheduleReceipts();
            if (!fresh->empty()) emit chatChanged();
            if (elsewhere > 0) emit incoming(elsewhere);
        },
        [this](const Failure& failure) {
            chatReceiving_ = false;
            if (receiveAgain_) {
                receiveAgain_ = false;
                QTimer::singleShot(1000, this, &DeviceKeysSession::receiveNow);
            }
            if (failure.status == 401) emit sessionExpired();
        });
}

// --- Calls over device keys (part B, docs/signal-integration/09-part-b-calls.md) ---

std::vector<e2ev3::ChatRecord> DeviceKeysSession::receiveWithCalls(e2ev3::V3Device& device, e2ev3::ChatHistory& history, CallInbox* inbox,
                                                                   e2ev3::ReceiveStats* stats) {
    std::vector<e2ev3::CallMessage> calls;
    std::shared_ptr<e2ev3::StoryStore> stories;
    if (inbox) {
        std::lock_guard lock(inbox->mutex);
        stories = inbox->stories;
    }
    std::set<std::string> before;
    if (stories) {
        for (const auto& k : stories->keys()) before.insert(k.storyId);
    }
    auto fresh = e2ev3::receiveChat(device, history, 0, stats, inbox ? &calls : nullptr, stories.get());
    if (stories) {
        std::vector<std::string> arrived;
        for (const auto& k : stories->keys()) {
            if (!before.count(k.storyId)) arrived.push_back(k.storyId);
        }
        if (!arrived.empty()) {
            {
                std::lock_guard lock(inbox->mutex);
                inbox->storyKeys.insert(inbox->storyKeys.end(), arrived.begin(), arrived.end());
            }
            if (inbox->notifyStories) inbox->notifyStories();
        }
    }
    if (inbox && !calls.empty()) {
        {
            std::lock_guard lock(inbox->mutex);
            for (auto& c : calls) inbox->items.push_back(std::move(c));
        }
        if (inbox->notify) inbox->notify();
    }
    return fresh;
}

void DeviceKeysSession::dispatchCalls() {
    std::vector<e2ev3::CallMessage> fresh;
    {
        std::lock_guard lock(callInbox_->mutex);
        fresh.swap(callInbox_->items);
    }
    // The checks that need the calls going on: a known call, its other party or a member, the epoch order.
    for (const auto& m : callGate_.admit(std::move(fresh), QDateTime::currentMSecsSinceEpoch())) {
        if (m.signal && callSignalHandler_) {
            callSignalHandler_(IncomingCallSignal{m.signal->callId, m.conversationId, m.fromUserId, m.fromDeviceId, m.signal->step,
                                                  m.signal->kind, m.signal->sdp});
        } else if (m.key && callKeyHandler_) {
            callKeyHandler_(IncomingCallKey{m.key->callId, m.conversationId, m.fromUserId, m.key->epoch, m.key->key});
        }
    }
}

void DeviceKeysSession::expectCall(const std::string& callId, const std::string& conversationId, const std::string& peerUserId, bool caller) {
    callGate_.expectCall(callId, conversationId, peerUserId, caller ? e2ev3::CallGate::Role::Caller : e2ev3::CallGate::Role::Callee);
    // What arrived before the live event named the call goes now (queued: never inside the caller's own handler).
    QTimer::singleShot(0, this, &DeviceKeysSession::dispatchCalls);
}

void DeviceKeysSession::expectGroupCall(const std::string& callId, const std::string& conversationId) {
    // Part C2: a community's members don't list each other; its call's roster names who may send keys
    // (setGroupCallMembers). Until it arrives, keys for the call are held (CallGate holds an unknown call's for 20 s).
    if (communities_->find(conversationId)) return;
    std::vector<std::string> members;
    if (chatInfo_) {
        for (const auto& [userId, handle] : chatInfo_(conversationId).handles) members.push_back(userId);
    }
    callGate_.expectGroupCall(callId, conversationId, std::move(members));
    QTimer::singleShot(0, this, &DeviceKeysSession::dispatchCalls);
}

void DeviceKeysSession::forgetCall(const std::string& callId) {
    callGate_.forget(callId);
}

void DeviceKeysSession::setGroupCallMembers(const std::string& callId, const std::string& conversationId,
                                            const std::vector<std::string>& members) {
    if (!communities_->find(conversationId)) return;  // other groups: the chat's members, as before
    callGate_.expectGroupCall(callId, conversationId, members);
    QTimer::singleShot(0, this, &DeviceKeysSession::dispatchCalls);
}

void DeviceKeysSession::setCommunities(const std::vector<CommunityChat>& communities) {
    std::vector<e2ev3::CommunityInfo> infos;
    bool staff = false;
    for (const CommunityChat& c : communities) {
        e2ev3::CommunityInfo info;
        info.communityId = c.communityId;
        info.conversationId = c.conversationId;
        info.kind = c.kind == "channel" ? "channel" : "group";
        info.staff = c.role == "owner" || c.role == "admin" || c.role == "moderator";
        info.canPost = c.canPost;
        staff = staff || info.staff;
        infos.push_back(std::move(info));
    }
    communities_->setAll(infos);
    const bool becameStaff = staff && !hasStaffCommunity_;
    hasStaffCommunity_ = staff;
    // The open chat may have just become known as a community.
    const bool open = !chatId_.isEmpty() && communities_->find(chatId_.toStdString()).has_value();
    if (open != chatCommunity_) {
        chatCommunity_ = open;
        if (open) chatEligible_ = available();
        decideChat();
        emit chatChanged();
    }
    if (becameStaff) runMembershipTasks();
}

void DeviceKeysSession::membershipNow() {
    runMembershipTasks();
}

void DeviceKeysSession::runMembershipTasks() {
    auto device = device_;
    if (!device || !trusted() || !hasStaffCommunity_ || tasksRunning_ || torDown()) return;
    history();  // the communities file is attached with the history
    tasksRunning_ = true;
    auto result = std::make_shared<e2ev3::MembershipRound>();
    runInBackground(
        chatPool_, this, [device, result] { translated([&] { *result = e2ev3::runMembershipTasks(*device); }); },
        [this, result] {
            tasksRunning_ = false;
            // Counts only: who joined or left is never logged.
            if (result->admitted > 0 || result->rosters > 0) receiveNow();
        },
        [this](const Failure& failure) {
            tasksRunning_ = false;
            if (failure.status == 401) emit sessionExpired();
        });
}

std::string DeviceKeysSession::callPeerDevice(const std::string& callId) const {
    return callGate_.peerDevice(callId);
}

void DeviceKeysSession::sendCallSignal(const std::string& conversationId, const std::string& peerUserId, const std::string& callId,
                                       const std::string& step, const std::string& kind, const std::string& sdp, const std::string& to,
                                       std::function<void(const QString& error)> done) {
    auto device = device_;
    if (!device || !trusted()) {
        done(QStringLiteral("This computer isn't linked to your devices, so the call can't be set up."));
        return;
    }
    // Never queued for Tor: a call that can't be set up now fails, as it would without a connection.
    if (torDown()) {
        done(QStringLiteral("Tor isn't connected, so the call can't be set up."));
        return;
    }
    auto settled = std::make_shared<bool>(false);
    auto finish = [settled, done = std::move(done)](const QString& error) {
        if (*settled) return;
        *settled = true;
        done(error);
    };
    QTimer::singleShot(kCallSendTimeoutMs, this, [finish] { finish(QStringLiteral("The call couldn't be set up in time.")); });
    const e2ev3::CallSignal signal{callId, step, kind, sdp, to};
    runInBackground(
        callPool_, this, [device, conversationId, peerUserId, signal] { translated([&] { e2ev3::sendCallSignal(*device, conversationId, peerUserId, signal); }); },
        [finish] { finish({}); },
        [this, finish](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            finish(failure.message.isEmpty() ? QStringLiteral("Couldn't set up the call.") : failure.message);
        });
}

void DeviceKeysSession::sendCallKey(const std::string& conversationId, const std::string& callId, std::int64_t epoch,
                                    std::shared_ptr<const crypto::SecretBytes> key, std::vector<std::string> userIds,
                                    std::function<void(const QString& error)> done) {
    auto device = device_;
    if (!device || !trusted() || torDown()) {
        done(QStringLiteral("The call key couldn't be handed out."));
        return;
    }
    auto settled = std::make_shared<bool>(false);
    auto finish = [settled, done = std::move(done)](const QString& error) {
        if (*settled) return;
        *settled = true;
        done(error);
    };
    QTimer::singleShot(kCallSendTimeoutMs, this, [finish] { finish(QStringLiteral("The call key couldn't be handed out in time.")); });
    e2ev3::CallKey callKey{callId, epoch, std::move(key)};
    runInBackground(
        callPool_, this,
        [device, conversationId, callKey, userIds = std::move(userIds)] {
            translated([&] { e2ev3::sendCallKey(*device, conversationId, callKey, userIds); });
        },
        [finish] { finish({}); },
        [this, finish](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            finish(failure.message.isEmpty() ? QStringLiteral("Couldn't hand out the call key.") : failure.message);
        });
}

// --- Stories over device keys (Part D, docs/signal-integration/12-part-d-stories.md) ---

std::shared_ptr<e2ev3::StoryStore> DeviceKeysSession::storyStore() {
    if (!history()) return nullptr;
    std::lock_guard lock(storyPart_->mutex);
    return storyPart_->store;
}

namespace {

/// Where story messages look up device lists, Signal certificates and one-time keys: under the story id itself
/// (the server's story rule, docs/signal-integration/12-part-d-stories.md). The one place to change it.
e2ev3::StoryFanOut storyRoutes() {
    e2ev3::StoryFanOut out;
    out.listVia = [](const std::string& storyId, const std::string&) { return storyId; };
    return out;  // claims: the message's own conversation, the story id
}

}  // namespace

e2ev3::StoryFanOut DeviceKeysSession::storyFanOut() const {
    return storyRoutes();
}

void DeviceKeysSession::announceStoryKeys() {
    std::vector<std::string> ids;
    {
        std::lock_guard lock(callInbox_->mutex);
        ids.swap(callInbox_->storyKeys);
    }
    if (ids.empty()) return;
    QStringList list;
    for (const auto& id : ids) list.append(qs(id));
    emit storyKeysArrived(list);
}

bool DeviceKeysSession::storiesOnDeviceKeys() const {
    return enabled_ && trusted() && device_ != nullptr;
}

std::shared_ptr<const crypto::SecretBytes> DeviceKeysSession::storyKey(const std::string& storyId, const std::string& authorId) {
    std::shared_ptr<e2ev3::StoryStore> store;
    {
        std::lock_guard lock(storyPart_->mutex);
        store = storyPart_->store;
    }
    if (!store) return nullptr;
    const auto key = store->key(storyId, authorId);
    return key ? key->key : nullptr;
}

void DeviceKeysSession::forgetStory(const std::string& storyId) {
    std::shared_ptr<e2ev3::StoryStore> store;
    {
        std::lock_guard lock(storyPart_->mutex);
        store = storyPart_->store;
    }
    if (store) store->forget(storyId);
}

std::vector<StoryDeviceKeys::HeldKey> DeviceKeysSession::heldStoryKeys() {
    std::shared_ptr<e2ev3::StoryStore> store;
    {
        std::lock_guard lock(storyPart_->mutex);
        store = storyPart_->store;
    }
    std::vector<HeldKey> out;
    if (!store) return out;
    for (const auto& k : store->keys()) out.push_back(HeldKey{k.storyId, k.authorId, k.own, k.arrivedAt});
    return out;
}

std::vector<StoryDeviceKeys::NamedAnswer> DeviceKeysSession::storyAnswers(const std::string& storyId) {
    std::shared_ptr<e2ev3::StoryStore> store;
    {
        std::lock_guard lock(storyPart_->mutex);
        store = storyPart_->store;
    }
    std::vector<NamedAnswer> out;
    if (!store) return out;
    for (const auto& a : store->answers(storyId)) out.push_back(NamedAnswer{a.stickerId, a.userId, a.rev, a.final, a.payload, a.at});
    return out;
}

void DeviceKeysSession::sendStoryKey(const std::string& storyId, std::shared_ptr<const crypto::SecretBytes> key, std::int64_t expiresAt,
                                     std::vector<std::string> viewers, std::function<void(const QString& error)> done) {
    auto device = device_;
    auto store = storyStore();
    if (!device || !store || !trusted() || !key) {
        if (done) done(QStringLiteral("This computer isn't linked to your devices, so the story's key couldn't be sent."));
        return;
    }
    const bool offline = torDown();
    const auto fanOut = storyFanOut();
    runInBackground(
        chatPool_, this,
        [device, store, storyId, key, expiresAt, viewers = std::move(viewers), offline, fanOut] {
            translated([&] { e2ev3::sendStoryKey(*device, *store, storyId, *key, expiresAt, viewers, fanOut, offline); });
        },
        [this, done] {
            if (done) done({});
            drainQueued();  // only if Tor came back meanwhile
        },
        [this, done](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            if (done) done(failure.message.isEmpty() ? QStringLiteral("The story's key couldn't be sent. It's tried again.") : failure.message);
        });
}

void DeviceKeysSession::sendStoryAnswer(const std::string& storyId, const std::string& authorId, const std::string& stickerId, bool final,
                                        std::string payloadJson, std::int64_t expiresAt, std::function<void(const QString& error)> done) {
    auto device = device_;
    auto store = storyStore();
    if (!device || !store || !trusted()) {
        if (done) done(QStringLiteral("This computer isn't linked to your devices yet. Link it in Settings > Devices & encryption."));
        return;
    }
    const Json payload = Json::parse(payloadJson, nullptr, false);
    if (!payload.is_object()) {
        if (done) done(QStringLiteral("That answer can't be sent."));
        return;
    }
    const bool offline = torDown();
    const auto fanOut = storyFanOut();
    runInBackground(
        chatPool_, this,
        [device, store, storyId, authorId, stickerId, final, payload, expiresAt, offline, fanOut] {
            translated([&] { e2ev3::sendStoryAnswer(*device, *store, storyId, authorId, stickerId, final, payload, expiresAt, fanOut, offline); });
        },
        [done] {
            if (done) done({});
        },
        [this, done](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            if (done) done(failure.code == QStringLiteral("answer_final") ? QStringLiteral("You already answered that.")
                           : failure.message.isEmpty()                    ? QStringLiteral("Couldn't send. Try again.")
                                                                          : failure.message);
        });
}

void DeviceKeysSession::fillStoryGaps(std::vector<std::pair<std::string, std::vector<std::string>>> gaps,
                                      std::function<void(const std::string& storyId, const std::vector<std::string>& userIds)> addAccess) {
    auto device = device_;
    auto store = storyStore();
    if (!device || !store || !trusted() || gaps.empty() || torDown()) return;
    std::vector<e2ev3::StoryGap> list;
    for (auto& [storyId, users] : gaps) list.push_back(e2ev3::StoryGap{storyId, users});
    const auto fanOut = storyFanOut();
    runInBackground(
        chatPool_, this,
        [device, store, list, addAccess = std::move(addAccess), fanOut] { translated([&] { e2ev3::fillStoryGaps(*device, *store, list, addAccess, fanOut); }); },
        [] {},
        [this](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            // Tried again with the next list of gaps.
        });
}

crypto::Bytes DeviceKeysSession::sealStoryAnswer(const std::string& storyId, const std::string& authorId, const model::stories::Sticker& sticker,
                                                 const model::stories::Answer& answer) {
    std::shared_ptr<e2ev3::V3Device> device;
    {
        std::lock_guard lock(storyPart_->mutex);
        device = storyPart_->device;
    }
    if (!device) throw api::UserFacingError("This computer isn't linked to your devices yet. Link it in Settings > Devices & encryption.");
    return e2ev3::sealStoryAnswer(storyId, authorId, sticker, answer, e2ev3::engineSealer(*device, storyRoutes()));
}

std::optional<model::stories::Answer> DeviceKeysSession::openStoryAnswer(const std::string& storyId, const model::stories::Sticker& sticker,
                                                                         const crypto::Bytes& rvsd) {
    std::shared_ptr<e2ev3::V3Device> device;
    {
        std::lock_guard lock(storyPart_->mutex);
        device = storyPart_->device;
    }
    if (!device) return std::nullopt;
    return e2ev3::openStoryAnswer(device->deviceId(), storyId, sticker, rvsd, e2ev3::engineOpener(*device));
}

void DeviceKeysSession::sendStoryReply(const std::string& chatId, const std::string& authorId, const std::string& text, const model::StoryReplyRef& ref,
                                       std::function<void(const QString& error)> done) {
    auto device = device_;
    auto h = history();
    if (!device || !h || !trusted()) {
        if (done) done(QStringLiteral("This computer isn't linked to your devices yet, so nothing was sent. Link it in Settings > Devices & encryption."));
        return;
    }
    e2ev3::OutgoingMessage message;
    message.text = text;
    message.extras.storyReply = ref;
    // The 80-character limit on a story's preview (it travels with the reply).
    std::string preview;
    for (std::size_t i = 0; i < ref.preview.size();) {
        const auto c = static_cast<unsigned char>(ref.preview[i]);
        const std::size_t width = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        if (e2ev3::textLength(preview + ref.preview.substr(i, width)) > 80) break;
        preview += ref.preview.substr(i, width);
        i += width;
    }
    message.extras.storyReply->preview = preview;
    if (chatInfo_) message.extras.exp = chatInfo_(chatId).timerSeconds;  // your timer in that chat, as for any message there
    const bool offline = torDown();
    runInBackground(
        chatPool_, this,
        [device, h, inbox = callInbox_, server = queueing_, offline, chatId, authorId, message] {
            translated([&] {
                if (offline || e2ev3::hasQueuedChat(*h, *server)) {
                    e2ev3::queueChatMessage(*device, *h, chatId, authorId, message);
                    return;
                }
                receiveWithCalls(*device, *h, inbox.get());
                e2ev3::sendChatMessage(*device, *h, chatId, authorId, message);
            });
        },
        [this, chatId, done] {
            if (qs(chatId) == chatId_) {
                reloadChat();
                emit chatChanged();
            }
            drainQueued();
            if (done) done({});
        },
        [this, chatId, done](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            if (qs(chatId) == chatId_) {
                reloadChat();
                emit chatChanged();
            }
            if (done) done(failure.message.isEmpty() ? QStringLiteral("Couldn't send. Try again.") : failure.message);
        });
}

#endif

}  // namespace redvault::app

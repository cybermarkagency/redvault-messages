// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "chat_session.hpp"

#include "prefs.hpp"

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QStandardPaths>

#include <algorithm>
#include <system_error>

#include "chat_images.hpp"
#include "file_safety.hpp"
#include "local_store.hpp"
#include "tor_controller.hpp"
#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/safety_number_qr.hpp"
#include "redvault/model/file_message.hpp"
#include "redvault/model/rich_message.hpp"
#include "redvault/model/text_format.hpp"
#include "redvault/util/private_files.hpp"
#include "redvault/util/text.hpp"

namespace redvault::app {

namespace fs = std::filesystem;

namespace {

constexpr int kConversationsPollMs = 5'000;
constexpr int kThreadPollMs = 3'000;
// A contact's username or key can change; re-resolve them now and then.
constexpr std::int64_t kMetaRefreshMs = 60'000;

const std::string kPaperclip = "\xF0\x9F\x93\x8E ";

fs::path pathFromUrl(const QUrl& url) {
    const QString local = url.isLocalFile() ? url.toLocalFile() : url.toString();
    return fs::path(local.toStdWString());
}

QString displayPath(const fs::path& path) {
    return QDir::toNativeSeparators(QString::fromStdWString(path.wstring()));
}

// What else goes when `message` is deleted for everyone: its edits and your
// own pin notices for it, since both repeat its text and apps that don't fold
// them would keep showing it.
std::vector<std::string> deletedWith(const data::Message& message, const std::vector<data::Message>& all) {
    std::vector<std::string> ids = message.editMessageIds;
    const std::string target = util::toLowerAscii(message.id);
    for (const auto& m : all) {
        if (m.fromMe && m.kind == data::MessageKind::Notice && util::toLowerAscii(m.noticeTarget) == target) ids.push_back(m.id);
    }
    return ids;
}

}  // namespace

ChatSession::ChatSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity,
                         TorController* tor, QObject* parent)
    : QObject(parent),
      api_(std::move(api)),
      identity_(std::move(identity)),
      // Runs on worker threads: QSettings instances are per call, so that's safe.
      keys_(std::make_shared<data::MessageKeys>(api_, &prefs::pinSigningKey, prefs::keyPinStore(), &prefs::pinnedSigningKey, prefs::contactKeyPinStore())),
      communityKeys_(std::make_shared<data::CommunityKeyStore>(api_, identity_)),
      repository_(std::make_shared<data::ConversationRepository>(
          api_,
          [](const std::string& selfUserId) {
              std::set<std::string> ids;
              for (const QString& id : prefs::hiddenMessages(selfUserId)) ids.insert(id.toStdString());
              return ids;
          },
          keys_,
          communityKeys_)),
      groups_(std::make_shared<data::GroupRepository>(api_, keys_)) {
    keys_->begin(*identity_);
    // Decrypted files go in a folder of their own for this session, removed at sign-out.
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    cacheDir_ = fs::path(cacheRoot.toStdWString()) / ("session-" + util::randomUuid());
    files_ = std::make_shared<data::FileRepository>(api_, repository_, cacheDir_);

    // A few parallel requests at most: polls, a send, a transfer and a mark-read can overlap.
    pool_.setMaxThreadCount(4);

    // Tor routing: every send goes through a sealed outbox first (it survives a
    // crash or a lost answer) and leaves only over Tor. Its key comes from the
    // account's private key, so it opens only in a signed-in session.
    if (tor != nullptr && tor->enabled()) {
        tor_ = tor;
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const fs::path dir = fs::path(root.toStdWString()) / "outbox" / identity_->userId;
        outbox_ = std::make_shared<data::Outbox>(dir, data::deriveOutboxKey(identity_->privateKey));
        connect(tor_, &TorController::becameConnected, this, &ChatSession::drainOutbox);
        drainTimer_.setInterval(15000);
        connect(&drainTimer_, &QTimer::timeout, this, &ChatSession::drainOutbox);
        drainTimer_.start();
        publishQueue();
    }
    connect(this, &ChatSession::currentChatChanged, this, [this] {
        shownKeyProblem_ = currentKeyProblem();
        emit keyProblemChanged();
    });
    listTimer_.setInterval(kConversationsPollMs);
    threadTimer_.setInterval(kThreadPollMs);
    connect(&listTimer_, &QTimer::timeout, this, &ChatSession::refreshAll);
    connect(&threadTimer_, &QTimer::timeout, this, &ChatSession::refreshThread);
    connect(&chatList_, &QAbstractItemModel::dataChanged, this, [this] { updateUnreadTotal(); });
    connect(&chatList_, &QAbstractItemModel::modelReset, this, [this] { updateUnreadTotal(); });
    connect(this, &ChatSession::localPrefsChanged, this, [this] { updateUnreadTotal(); });
    listTimer_.start();
    refreshAll();
}

ChatSession::~ChatSession() {
    listTimer_.stop();
    threadTimer_.stop();
    // Queued work is dropped; running calls finish (their results are discarded) before the keys go.
    pool_.clear();
    pool_.waitForDone();
    keys_->end();
    ChatImageCache::instance().clear();
    std::error_code ignored;
    fs::remove_all(cacheDir_, ignored);
}

// --- Current chat ---

const data::ConversationMeta* ChatSession::currentMeta() const {
    const auto it = metas_.find(currentChatId_);
    return it == metas_.end() ? nullptr : it->second.get();
}

std::shared_ptr<const data::ConversationMeta> ChatSession::currentMetaShared() const {
    const auto it = metas_.find(currentChatId_);
    return it == metas_.end() ? nullptr : it->second;
}

std::shared_ptr<const data::ConversationMeta> ChatSession::metaFor(const std::string& chatId) const {
    const auto it = metas_.find(chatId);
    return it == metas_.end() ? nullptr : it->second;
}

QString ChatSession::currentUserId() const {
    const auto* meta = currentMeta();
    return meta && !meta->isGroup ? QString::fromStdString(meta->otherUserId) : QString();
}

bool ChatSession::contactKeyChanged(const data::ConversationMeta& meta) const {
    if (meta.isGroup || meta.otherPublicKeyB64.empty()) return false;
    if (local_store::isVerified(identity_->userId, meta.otherPublicKeyB64)) return false;
    return prefs::hasKeyChanged(identity_->userId, meta.otherUserId, meta.otherPublicKeyB64);
}

bool ChatSession::currentKeyChanged() const {
    const auto* meta = currentMeta();
    return meta != nullptr && contactKeyChanged(*meta);
}

QString ChatSession::currentKeyProblem() const {
    const auto problem = currentChatId_.empty() ? std::nullopt : repository_->keyProblem(currentChatId_);
    return problem ? QString::fromStdString(*problem) : QString();
}

QString ChatSession::sendRefusal(const std::string& chatId, const char* onDeviceKeys) const {
    if (const auto problem = repository_->keyProblem(chatId)) return QString::fromStdString(*problem);
    // Account keys never change in normal use, so a new one holds sending until it's verified.
    if (const auto meta = metaFor(chatId); meta && contactKeyChanged(*meta)) {
        return QStringLiteral("Sending is paused because this contact's safety number changed. Verify it first.");
    }
    if (chatPinnedToDeviceKeys_ && chatPinnedToDeviceKeys_(chatId)) {
        const QString reason = deviceKeyReason_ ? deviceKeyReason_(chatId) : QString();
        return reason.isEmpty() ? QString::fromUtf8(onDeviceKeys) : reason;
    }
    return {};
}

QVariantList ChatSession::contacts() const {
    QVariantList out;
    for (const auto& [id, meta] : metas_) {
        if (meta->isGroup || meta->otherUserId.empty()) continue;
        const QString handle = QString::fromStdString(meta->handle);
        out.append(QVariantMap{
            {"chatId", QString::fromStdString(meta->id)},
            {"userId", QString::fromStdString(meta->otherUserId)},
            {"handle", handle},
            {"initial", handle.isEmpty() ? QStringLiteral("?") : handle.left(1).toUpper()},
            {"avatarUrl", QString::fromStdString(meta->avatarUrl.value_or(""))},
            {"verified", local_store::isVerified(identity_->userId, meta->otherPublicKeyB64)},
            {"safetyNumber", QString::fromStdString([&] {
                 std::string joined;
                 for (const auto& group : meta->safetyNumber) joined += (joined.empty() ? "" : " ") + group;
                 return joined;
             }())},
        });
    }
    std::sort(out.begin(), out.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("handle").toString().compare(b.toMap().value("handle").toString(), Qt::CaseInsensitive) < 0;
    });
    return out;
}

QStringList ChatSession::favorites() const {
    return prefs::favorites(identity_->userId);
}

QStringList ChatSession::muted() const {
    return prefs::muted(identity_->userId);
}

void ChatSession::toggleFavorite(const QString& userId) {
    prefs::toggleIn(identity_->userId, "favorites", userId);
    emit localPrefsChanged();
}

void ChatSession::toggleMuted(const QString& userId) {
    prefs::toggleIn(identity_->userId, "muted", userId);
    emit localPrefsChanged();
}

QString ChatSession::chatIdForUser(const QString& userId) const {
    const std::string id = userId.toStdString();
    for (const auto& [chatId, meta] : metas_) {
        if (!meta->isGroup && meta->otherUserId == id) return QString::fromStdString(chatId);
    }
    return {};
}

bool ChatSession::isVerifiedUser(const QString& userId) const {
    const std::string id = userId.toStdString();
    for (const auto& [chatId, meta] : metas_) {
        if (!meta->isGroup && meta->otherUserId == id) return local_store::isVerified(identity_->userId, meta->otherPublicKeyB64);
    }
    return false;
}

std::vector<std::shared_ptr<const data::ConversationMeta>> ChatSession::metas() const {
    std::vector<std::shared_ptr<const data::ConversationMeta>> out;
    for (const auto& [id, meta] : metas_) out.push_back(meta);
    return out;
}

QString ChatSession::currentTitle() const {
    const auto* meta = currentMeta();
    return meta ? QString::fromStdString(meta->handle) : QString();
}

QString ChatSession::currentSubtitle() const {
    const auto* meta = currentMeta();
    if (!meta) return {};
    if (meta->community) {
        return QStringLiteral("%1 · end-to-end encrypted%2")
            .arg(meta->community->kind == "channel" ? QStringLiteral("Channel") : QStringLiteral("Community"),
                 meta->community->canPost ? QString() : QStringLiteral(" · read only"));
    }
    if (meta->isGroup) return QStringLiteral("%1 members").arg(meta->members.size());
    return currentVerified() ? QStringLiteral("Verified · end-to-end encrypted") : QStringLiteral("End-to-end encrypted");
}

bool ChatSession::currentIsGroup() const {
    const auto* meta = currentMeta();
    return meta && meta->isGroup;
}

QString ChatSession::currentCommunityId() const {
    const auto* meta = currentMeta();
    return meta && meta->community ? QString::fromStdString(meta->community->id) : QString();
}

QString ChatSession::currentCommunityKind() const {
    const auto* meta = currentMeta();
    return meta && meta->community ? QString::fromStdString(meta->community->kind) : QString();
}

QString ChatSession::currentCommunityImage() const {
    const auto* meta = currentMeta();
    return meta ? communityAvatarPath(*meta) : QString();
}

bool ChatSession::currentCanPost() const {
    const auto* meta = currentMeta();
    return !meta || !meta->community || meta->community->canPost;
}

QString ChatSession::senderOf(const QString& messageId) const {
    const auto* message = messages_.find(messageId.toStdString());
    return message ? QString::fromStdString(message->senderId) : QString();
}

QString ChatSession::currentMyRole() const {
    const auto* meta = currentMeta();
    return meta ? QString::fromStdString(meta->myRole) : QString();
}

QVariantList ChatSession::currentMembers() const {
    QVariantList out;
    const auto* meta = currentMeta();
    if (!meta) return out;
    for (const auto& member : meta->members) {
        out.append(QVariantMap{{QStringLiteral("userId"), QString::fromStdString(member.userId)},
                               {QStringLiteral("handle"), QString::fromStdString(member.handle)},
                               {QStringLiteral("role"), QString::fromStdString(member.role)},
                               {QStringLiteral("avatarUrl"), QString::fromStdString(member.avatarUrl.value_or(""))},
                               {QStringLiteral("isSelf"), member.isSelf}});
    }
    return out;
}

QString ChatSession::currentSafetyNumber() const {
    const auto* meta = currentMeta();
    if (!meta || meta->isGroup) return {};
    // 12 groups of 5 digits, shown as 3 rows of 4, like the other apps.
    QString out;
    for (std::size_t i = 0; i < meta->safetyNumber.size(); ++i) {
        if (i > 0) out += (i % 4 == 0) ? u'\n' : u' ';
        out += QString::fromStdString(meta->safetyNumber[i]);
    }
    return out;
}

QString ChatSession::currentSafetyQr() const {
    const auto* meta = currentMeta();
    if (!meta || meta->isGroup || meta->safetyNumber.empty()) return {};
    return QString::fromStdString(crypto::safety_qr::encode(meta->safetyNumber));
}

QString ChatSession::currentAvatarUrl() const {
    const auto* meta = currentMeta();
    return meta && !meta->isGroup ? QString::fromStdString(meta->avatarUrl.value_or("")) : QString();
}

bool ChatSession::currentVerified() const {
    const auto* meta = currentMeta();
    return meta && !meta->isGroup && local_store::isVerified(identity_->userId, meta->otherPublicKeyB64);
}

void ChatSession::setCurrentVerified(bool verified) {
    const auto* meta = currentMeta();
    if (!meta || meta->isGroup) return;
    local_store::setVerified(identity_->userId, meta->otherPublicKeyB64, verified);
    // Verifying the new key is what clears a key-change warning.
    if (verified) prefs::acceptContactKey(identity_->userId, meta->otherUserId, meta->otherPublicKeyB64);
    emit contactsChanged();
    emit currentChatChanged();
}

void ChatSession::openChat(const QString& chatId) {
    if (chatId.toStdString() != currentChatId_) threadWindow_ = data::ConversationRepository::kPageSize;
    currentChatId_ = chatId.toStdString();
    const auto* chat = chatList_.find(currentChatId_);
    if (chat) {
        showThread(chat->meta, withKnownSecurity(chat->messages));
    } else {
        messages_.clear();
    }
    setHistory(chat != nullptr && chat->hasOlder, false);
    emit currentChatChanged();
    threadTimer_.start();
    refreshThread();
}

void ChatSession::openWhenListed(const std::string& chatId) {
    if (chatId.empty()) return;
    if (metas_.contains(chatId)) {
        openChat(QString::fromStdString(chatId));
        return;
    }
    pendingOpenChatId_ = chatId;
    refreshAll();
}

void ChatSession::groupChanged(const std::string& chatId) {
    staleMetas_.insert(chatId);
    refreshAll();
}

void ChatSession::closeChat() {
    currentChatId_.clear();
    threadWindow_ = data::ConversationRepository::kPageSize;
    threadTimer_.stop();
    messages_.clear();
    setHistory(false, false);
    emit currentChatChanged();
}

void ChatSession::loadOlder() {
    if (currentChatId_.empty() || !hasOlder_ || loadingOlder_) return;
    threadWindow_ += data::ConversationRepository::kPageSize;
    setHistory(hasOlder_, true);
    // If a poll is already running, it fetches the bigger window when it lands (see applyThread).
    refreshThread();
}

void ChatSession::setHistory(bool hasOlder, bool loadingOlder) {
    if (hasOlder_ == hasOlder && loadingOlder_ == loadingOlder) return;
    hasOlder_ = hasOlder;
    loadingOlder_ = loadingOlder;
    emit historyChanged();
}

void ChatSession::updateUnreadTotal() {
    const QStringList muted = prefs::muted(identity_->userId);
    int total = 0;
    for (const auto& chat : chatList_.chats()) {
        if (!chat.meta->isGroup && muted.contains(QString::fromStdString(chat.meta->otherUserId))) continue;
        total += chat.unread;
    }
    if (total == unreadTotal_) return;
    unreadTotal_ = total;
    emit unreadTotalChanged();
}

void ChatSession::refreshNow() {
    refreshAll();
}

void ChatSession::clearError() {
    setError({});
}

// --- Polling ---

void ChatSession::refreshAll() {
    if (refreshingList_) return;
    refreshingList_ = true;
    auto repository = repository_;
    auto identity = identity_;
    auto cached = metas_;
    for (const auto& id : staleMetas_) cached.erase(id);
    staleMetas_.clear();
    for (const auto& request : requests_) cached.emplace(request.meta->id, request.meta);
    runInBackground(
        pool_, this,
        [repository, identity, cached]() {
            RefreshResult result;
            const auto now = util::nowMillis();
            // Message requests: decrypted to show who and what, but never marked read.
            try {
                for (const auto& dto : repository->listRequests()) {
                    if (dto.isGroup) continue;
                    std::shared_ptr<const data::ConversationMeta> meta;
                    if (const auto it = cached.find(dto.id); it != cached.end()) {
                        meta = it->second;
                    } else {
                        try {
                            auto hydrated = repository->hydrateMeta(dto, *identity);
                            if (!hydrated) continue;
                            meta = std::make_shared<const data::ConversationMeta>(std::move(*hydrated));
                        } catch (const crypto::CryptoError&) {
                            continue;
                        }
                    }
                    auto messages = repository->fetchMessages(*meta, identity->userId);
                    result.requests.push_back(ChatSummary{meta, std::move(messages.messages), messages.unread, dto.disappearingSeconds, messages.hasOlder, {}});
                }
            } catch (const api::ApiError& error) {
                if (error.status() == 401) throw;  // the session ended: handled like any other 401
            }
            for (const auto& dto : repository->listConversations()) {
                // One chat that can't be opened or loaded is left out of this refresh; the others still load.
                if (repository->isCommunity(dto)) {
                    // A community: hydrated every round (its keys are reloaded only when the epoch
                    // moves), then named after whoever wrote what's loaded. One that can't be read
                    // right now (just removed, say) is left out rather than failing the whole list.
                    try {
                        auto hydrated = repository->hydrateMeta(dto, *identity);
                        if (!hydrated) continue;
                        auto messages = repository->fetchMessages(*hydrated, identity->userId);
                        repository->addCommunitySenders(*hydrated, messages.messages);
                        auto meta = std::make_shared<const data::ConversationMeta>(std::move(*hydrated));
                        result.metas[dto.id] = meta;
                        result.chats.push_back(ChatSummary{meta, std::move(messages.messages), messages.unread,
                                                           dto.disappearingSeconds, messages.hasOlder, std::move(messages.pinned)});
                    } catch (const api::ApiError& error) {
                        if (error.status() == 401) throw;
                    } catch (const crypto::CryptoError&) {
                    }
                    continue;
                }
                try {
                    std::shared_ptr<const data::ConversationMeta> meta;
                    if (const auto it = cached.find(dto.id); it != cached.end() && now - it->second->fetchedAtMs < kMetaRefreshMs) {
                        meta = it->second;
                        // A group's key can change at any time (someone left): keep its keys current.
                        repository->refreshKeys(dto);
                    } else {
                        auto hydrated = repository->hydrateMeta(dto, *identity);
                        if (!hydrated) continue;  // not a well-formed chat any more
                        meta = std::make_shared<const data::ConversationMeta>(std::move(*hydrated));
                    }
                    auto messages = repository->fetchMessages(*meta, identity->userId);
                    result.metas[dto.id] = meta;
                    result.chats.push_back(ChatSummary{meta, std::move(messages.messages), messages.unread,
                                                       dto.disappearingSeconds, messages.hasOlder, std::move(messages.pinned)});
                } catch (const crypto::CryptoError&) {
                    continue;  // a key this account can't open: nothing to show
                } catch (const api::ApiError& error) {
                    if (error.status() == 401) throw;  // the session ended: handled like any other 401
                    continue;
                }
            }
            return result;
        },
        [this](RefreshResult result) {
            refreshingList_ = false;
            metas_ = std::move(result.metas);
            if (const QString problem = currentKeyProblem(); problem != shownKeyProblem_) {
                shownKeyProblem_ = problem;
                emit keyProblemChanged();
            }
            requests_ = std::move(result.requests);
            emit messageRequestsChanged();
            for (auto& chat : result.chats) chat.messages = withKnownSecurity(std::move(chat.messages));
            // The open chat's own poll is fresher about its unread count, and
            // may hold older history than this fetched: keep that part.
            if (!currentChatId_.empty()) {
                for (auto& chat : result.chats) {
                    if (chat.meta->id != currentChatId_) continue;
                    chat.unread = 0;
                    const auto* shown = chatList_.find(currentChatId_);
                    if (shown == nullptr || threadWindow_ <= data::ConversationRepository::kPageSize) continue;
                    const std::int64_t oldestFresh = chat.messages.empty() ? INT64_MAX : chat.messages.front().createdAtMs;
                    std::vector<data::Message> merged;
                    for (const auto& message : shown->messages) {
                        if (message.createdAtMs < oldestFresh) merged.push_back(message);
                    }
                    merged.insert(merged.end(), std::make_move_iterator(chat.messages.begin()),
                                  std::make_move_iterator(chat.messages.end()));
                    chat.messages = std::move(merged);
                    chat.hasOlder = shown->hasOlder;
                    // Pins come from the history loaded, which is more here than the list fetched.
                    chat.pinned = shown->pinned;
                }
            }
            for (const auto& chat : result.chats) noticeIncoming(chat);
            incomingBaseline_ = true;
            chatList_.setChats(std::move(result.chats));
            emit contactsChanged();
            if (!loaded_) {
                loaded_ = true;
                emit loadedChanged();
            }
            setError({});
            // A group just created or a request just accepted: open it once it's listed.
            if (!pendingOpenChatId_.empty() && metas_.contains(pendingOpenChatId_)) {
                const std::string id = std::exchange(pendingOpenChatId_, {});
                openChat(QString::fromStdString(id));
                return;
            }
            if (!currentChatId_.empty() && !metas_.contains(currentChatId_)) {
                closeChat();  // left or removed
                return;
            }
            // Keep the open chat's view in step with its (possibly re-resolved) meta.
            if (const auto* chat = chatList_.find(currentChatId_)) showThread(chat->meta, withKnownSecurity(chat->messages));
            emit currentChatChanged();
        },
        [this](const Failure& failure) {
            refreshingList_ = false;
            fail(failure);
        });
}

void ChatSession::refreshThread() {
    if (refreshingThread_ || currentChatId_.empty()) return;
    const auto meta = currentMetaShared();
    if (!meta) return;
    refreshingThread_ = true;
    auto repository = repository_;
    const std::string userId = identity_->userId;
    const std::string chatId = currentChatId_;
    const int window = threadWindow_;
    const bool readReceipts = local_store::readReceipts();
    runInBackground(
        pool_, this,
        [repository, meta, userId, window, readReceipts]() {
            auto result = repository->fetchMessages(*meta, userId, window);
            // "read" is visible to the sender; "delivered" only keeps our own unread count right.
            if (result.unread > 0) repository->markRead(meta->id, readReceipts);
            return result;
        },
        [this, chatId, window](data::MessagesResult result) {
            refreshingThread_ = false;
            applyThread(chatId, window, std::move(result));
        },
        [this](const Failure& failure) {
            refreshingThread_ = false;
            if (loadingOlder_) setHistory(hasOlder_, false);
            fail(failure);
        });
}

void ChatSession::applyThread(const std::string& chatId, int window, data::MessagesResult result) {
    auto messages = withKnownSecurity(std::move(result.messages));
    if (auto* chat = chatList_.find(chatId)) {
        const std::int64_t historyBefore = chat->messages.empty() ? 0 : chat->messages.front().createdAtMs;
        chat->messages = messages;
        chat->unread = 0;
        chat->hasOlder = result.hasOlder;
        const bool pinsChanged = chat->pinned.size() != result.pinned.size() ||
                                 !std::equal(chat->pinned.begin(), chat->pinned.end(), result.pinned.begin(),
                                             [](const data::PinnedView& a, const data::PinnedView& b) {
                                                 return a.messageId == b.messageId && a.atMs == b.atMs;
                                             });
        chat->pinned = std::move(result.pinned);
        if (pinsChanged && chatId == currentChatId_) emit currentChatChanged();
        noticeIncoming(*chat, historyBefore);
        // Re-sort and refresh the rows (this chat's preview and time may have moved on).
        chatList_.setChats(chatList_.snapshot());
    }
    if (chatId == currentChatId_) {
        if (const auto it = metas_.find(chatId); it != metas_.end()) showThread(it->second, std::move(messages));
        if (window < threadWindow_) {
            // "Load older" was asked for while this poll was out: fetch the bigger window now.
            setHistory(result.hasOlder, true);
            refreshThread();
        } else {
            setHistory(result.hasOlder, false);
        }
    }
    setError({});
}

// --- Sending ---

bool ChatSession::refuseSend(const std::string& chatId, const char* onDeviceKeys) {
    const QString why = sendRefusal(chatId, onDeviceKeys);
    if (why.isEmpty()) return false;
    setError(why);
    emit sent(false);
    return true;
}

void ChatSession::sendMessage(const QString& text, const QString& replyToId, const QString& entitlementId, bool safetyContact) {
    const auto meta = currentMetaShared();
    if (!meta || sending_) return;
    if (refuseSend(meta->id, device_key_refusal::kMessage)) return;
    std::optional<data::ReplyTarget> replyTo;
    if (!replyToId.isEmpty()) {
        if (const auto* target = messages_.find(replyToId.toStdString())) {
            const std::string senderId =
                target->fromMe ? identity_->userId : (target->senderId.empty() ? meta->otherUserId : target->senderId);
            const std::string preview = target->kind == data::MessageKind::File ? kPaperclip + target->text : target->text;
            replyTo = data::ReplyTarget{{target->id, senderId, preview}, target->fromMe};
        }
    }
    if (outbox_) {
        // Tor routing: sealed into the outbox first, then sent over Tor straight
        // away if it's connected. Until the server has it, it shows as queued.
        if (text.trimmed().isEmpty()) return;
        try {
            if (replyTo) {
                outbox_->addText(meta->id, text.toStdString(), replyTo->ref.messageId, replyTo->ref.preview, replyTo->fromMe,
                                 replyTo->ref.senderId);
            } else {
                outbox_->addText(meta->id, text.toStdString());
            }
        } catch (const std::exception&) {
            setError(QStringLiteral("Couldn't keep the message safe to send later. Nothing was sent."));
            emit sent(false);
            return;
        }
        emit sent(true);
        publishQueue();
        drainOutbox();
        return;
    }
    sending_ = true;
    emit sendingChanged();
    auto repository = repository_;
    const std::string body = text.toStdString();
    const std::string chatId = currentChatId_;
    data::PaidSend paid;
    if (!entitlementId.isEmpty()) paid.entitlementId = entitlementId.toStdString();
    paid.safetyContact = safetyContact;
    runInBackground(
        pool_, this, [repository, meta, body, replyTo, paid]() { return repository->send(*meta, body, replyTo, std::nullopt, paid); },
        [this, chatId](data::Message message) {
            sending_ = false;
            emit sendingChanged();
            if (message.security) securityById_[message.id] = *message.security;
            emit sent(true);
            // Show it right away; the next poll brings the server's view of it.
            if (auto* chat = chatList_.find(chatId)) {
                chat->messages.push_back(message);
                if (chatId == currentChatId_) showThread(chat->meta, chat->messages);
                chatList_.setChats(chatList_.snapshot());
            }
            refreshThread();
        },
        [this, chatId](const Failure& failure) {
            sending_ = false;
            emit sendingChanged();
            emit sent(false);
            // Paid messages: the earnings session shows what to do (pay first).
            if (failure.code == QStringLiteral("payment_required")) emit paymentRequired(QString::fromStdString(chatId));
            fail(failure);
        });
}

void ChatSession::react(const QString& messageId, const QString& emoji) {
    const auto meta = currentMetaShared();
    if (!meta) return;
    const auto* target = messages_.find(messageId.toStdString());
    if (!target) return;
    if (const QString why = sendRefusal(meta->id, device_key_refusal::kChange); !why.isEmpty()) return setError(why);
    auto repository = repository_;
    // Tapping your own reaction again removes it: deleting the reaction message is how it's undone.
    std::optional<std::string> existing;
    for (const auto& reaction : target->reactions) {
        if (reaction.emoji == emoji.toStdString() && reaction.mine) existing = reaction.myReactionId;
    }
    const std::string id = messageId.toStdString();
    const std::string chosen = emoji.toStdString();
    runInBackground(
        pool_, this,
        [repository, meta, id, chosen, existing]() {
            if (existing) {
                repository->deleteMessage(*existing);
            } else {
                repository->sendReaction(*meta, id, chosen);
            }
        },
        [this]() { refreshThread(); }, [this](const Failure& failure) { fail(failure); });
}

QVariantMap ChatSession::imageInfo(const QString& messageId) const {
    constexpr std::int64_t kAutoLoadBytes = 20LL * 1024 * 1024;
    const auto* message = messages_.find(messageId.toStdString());
    if (!message || !isInlineImage(*message)) return {};
    const auto it = imageStates_.find(message->id);
    QString state = it == imageStates_.end() ? QString() : it->second;
    if (state.isEmpty() && message->file->originalSize > kAutoLoadBytes) state = QStringLiteral("large");
    const QSize size = ChatImageCache::instance().thumbSize(messageId);
    return {{"state", state}, {"width", size.width()}, {"height", size.height()}};
}

void ChatSession::loadImage(const QString& messageId, bool force) {
    constexpr std::int64_t kAutoLoadBytes = 20LL * 1024 * 1024;
    const auto* message = messages_.find(messageId.toStdString());
    if (!message || !isInlineImage(*message)) return;
    const std::string id = message->id;
    if (const auto it = imageStates_.find(id); it != imageStates_.end() && it->second != QStringLiteral("failed")) {
        // Already loading or shown; a full view whose bytes were dropped still has its thumbnail.
        if (it->second == QStringLiteral("ready") && ChatImageCache::instance().has(messageId)) return;
        if (it->second == QStringLiteral("loading")) return;
    }
    if (!force && message->file->originalSize > kAutoLoadBytes) return;
    imageStates_[id] = QStringLiteral("loading");
    ++imagesRevision_;
    emit imagesChanged();
    auto files = files_;
    const auto descriptor = *message->file;
    struct Decoded {
        QImage thumb;
        QByteArray bytes;
        bool ok = false;
    };
    runInBackground(
        pool_, this,
        [files, descriptor]() {
            Decoded out;
            const auto path = files->download(descriptor);
            QFile file(QString::fromStdWString(path.wstring()));
            if (file.open(QIODevice::ReadOnly)) out.bytes = file.readAll();
            file.close();
            // The decrypted copy goes straight away: pictures are kept in memory only.
            std::error_code ignored;
            fs::remove_all(path.parent_path(), ignored);
            if (const auto image = ChatImageCache::decode(out.bytes)) {
                out.thumb = ChatImageCache::thumbnail(*image);
                out.ok = true;
            }
            return out;
        },
        [this, id, messageId](Decoded decoded) {
            if (decoded.ok) ChatImageCache::instance().put(messageId, decoded.thumb, decoded.bytes);
            imageStates_[id] = decoded.ok ? QStringLiteral("ready") : QStringLiteral("failed");
            ++imagesRevision_;
            emit imagesChanged();
        },
        [this, id](const Failure& failure) {
            imageStates_[id] = QStringLiteral("failed");
            ++imagesRevision_;
            emit imagesChanged();
            if (failure.status == 401) emit sessionExpired();
        });
}

QVariantList ChatSession::currentPinned() const {
    QVariantList out;
    const auto* chat = chatList_.find(currentChatId_);
    if (!chat) return out;
    for (auto it = chat->pinned.rbegin(); it != chat->pinned.rend(); ++it) {
        const QString by = it->pinnedBy == identity_->userId ? QStringLiteral("You") : QString::fromStdString(chat->meta->handleOf(it->pinnedBy));
        out.append(QVariantMap{{"messageId", QString::fromStdString(it->messageId)},
                               {"preview", QString::fromStdString(model::text_format::toPlain(it->preview))},
                               {"by", by}});
    }
    return out;
}

bool ChatSession::isPinned(const QString& messageId) const {
    const auto* chat = chatList_.find(currentChatId_);
    if (!chat) return false;
    const std::string id = messageId.toLower().toStdString();
    return std::any_of(chat->pinned.begin(), chat->pinned.end(), [&](const data::PinnedView& p) { return p.messageId == id; });
}

template <typename Work>
void ChatSession::sendControl(const char* onDeviceKeys, Work work) {
    const auto meta = currentMetaShared();
    if (!meta) return;
    if (const QString why = sendRefusal(meta->id, onDeviceKeys); !why.isEmpty()) return setError(why);
    auto repository = repository_;
    runInBackground(
        pool_, this, [repository, meta, work]() { work(*repository, *meta); }, [this]() { refreshThread(); },
        [this](const Failure& failure) { fail(failure); });
}

void ChatSession::pin(const QString& messageId) {
    const auto* message = messages_.find(messageId.toStdString());
    if (!message || message->kind == data::MessageKind::Notice || message->kind == data::MessageKind::Undecryptable) return;
    const std::string id = message->id;
    const std::string preview = message->kind == data::MessageKind::File ? kPaperclip + message->text
                              : message->kind == data::MessageKind::Poll ? "\xF0\x9F\x93\x8A " + message->text
                                                                         : message->text;
    sendControl(device_key_refusal::kPin, [id, preview](data::ConversationRepository& repo, const data::ConversationMeta& meta) { repo.sendPin(meta, id, preview); });
}

void ChatSession::unpin(const QString& messageId) {
    const std::string id = messageId.toStdString();
    sendControl(device_key_refusal::kPin, [id](data::ConversationRepository& repo, const data::ConversationMeta& meta) { repo.sendUnpin(meta, id); });
}

void ChatSession::createPoll(const QString& question, const QStringList& options, bool multiple) {
    const auto meta = currentMetaShared();
    if (!meta) return;
    if (const QString why = sendRefusal(meta->id, device_key_refusal::kPoll); !why.isEmpty()) {
        emit pollDone(why);
        return;
    }
    std::vector<std::string> labels;
    for (const auto& option : options) labels.push_back(option.toStdString());
    const std::string q = question.toStdString();
    try {
        (void)model::rich_message::serializePoll(q, labels, multiple);  // checked here so the dialog can say what's wrong
    } catch (const std::invalid_argument&) {
        emit pollDone(QStringLiteral("Write a question and 2 to 10 different options."));
        return;
    }
    auto repository = repository_;
    runInBackground(
        pool_, this, [repository, meta, q, labels, multiple]() { repository->sendPoll(*meta, q, labels, multiple); },
        [this]() {
            emit pollDone({});
            refreshThread();
        },
        [this](const Failure& failure) {
            emit pollDone(failure.message);
            if (failure.status == 401) emit sessionExpired();
        });
}

void ChatSession::vote(const QString& pollId, const QVariantList& options) {
    const auto* message = messages_.find(pollId.toStdString());
    if (!message || !message->poll || message->poll->closed) return;
    // Before the count on screen moves: a vote that can't go out mustn't look as if it did.
    if (const QString why = sendRefusal(currentChatId_, device_key_refusal::kVote); !why.isEmpty()) return setError(why);
    std::vector<int> picked;
    for (const auto& option : options) picked.push_back(option.toInt());
    const data::Message poll = *message;
    // Shown at once; the poll's next refresh brings everyone's votes.
    if (auto* chat = chatList_.find(currentChatId_)) {
        for (auto& m : chat->messages) {
            if (m.id != poll.id || !m.poll) continue;
            const bool had = !m.poll->mine.empty();
            for (const int old : m.poll->mine) --m.poll->counts[static_cast<std::size_t>(old)];
            m.poll->mine = picked;
            for (const int now : picked) ++m.poll->counts[static_cast<std::size_t>(now)];
            m.poll->voters += (picked.empty() ? 0 : 1) - (had ? 1 : 0);
        }
        showThread(chat->meta, chat->messages);
    }
    sendControl(device_key_refusal::kVote, [poll, picked](data::ConversationRepository& repo, const data::ConversationMeta& meta) { repo.sendVote(meta, poll, picked); });
}

void ChatSession::endPoll(const QString& pollId) {
    const auto* message = messages_.find(pollId.toStdString());
    if (!message || !message->poll || !message->fromMe || message->poll->closed) return;
    const std::string id = message->id;
    sendControl(device_key_refusal::kVote, [id](data::ConversationRepository& repo, const data::ConversationMeta& meta) { repo.sendEndPoll(meta, id); });
}

QVariantList ChatSession::messageRequests() const {
    QVariantList out;
    for (const auto& request : requests_) {
        const auto& meta = *request.meta;
        const QString handle = QString::fromStdString(meta.handle);
        QStringList texts;
        for (const auto& message : request.messages) {
            if (message.fromMe) continue;
            texts.append(message.kind == data::MessageKind::File ? QStringLiteral("\U0001F4CE ") + QString::fromStdString(message.text)
                                                                 : QString::fromStdString(message.text));
        }
        out.append(QVariantMap{
            {"chatId", QString::fromStdString(meta.id)},
            {"userId", QString::fromStdString(meta.otherUserId)},
            {"handle", handle},
            {"initial", handle.isEmpty() ? QStringLiteral("?") : handle.left(1).toUpper()},
            {"avatarUrl", QString::fromStdString(meta.avatarUrl.value_or(""))},
            // Their messages so far, oldest first, as plain text.
            {"messages", texts},
            {"preview", texts.isEmpty() ? QStringLiteral("No messages yet") : texts.last().simplified()},
            {"time", formatListTime(request.lastActivityMs())},
        });
    }
    return out;
}

void ChatSession::respondToRequest(const QString& chatId, bool accept) {
    auto repository = repository_;
    const std::string id = chatId.toStdString();
    runInBackground(
        pool_, this, [repository, id, accept]() { repository->respondToRequest(id, accept); },
        [this, id, accept]() {
            std::erase_if(requests_, [&](const ChatSummary& request) { return request.meta->id == id; });
            emit messageRequestsChanged();
            if (accept) pendingOpenChatId_ = id;
            refreshAll();
        },
        [this](const Failure& failure) { fail(failure); });
}

int ChatSession::currentDisappearing() const {
    const auto* chat = chatList_.find(currentChatId_);
    return chat ? data::Disappearing::fromSeconds(chat->disappearingSeconds).seconds.value_or(0) : 0;
}

QString ChatSession::currentDisappearingLabel() const {
    const auto* chat = chatList_.find(currentChatId_);
    const auto timer = data::Disappearing::fromSeconds(chat ? chat->disappearingSeconds : std::nullopt);
    return QString::fromUtf8(timer.label.data(), static_cast<qsizetype>(timer.label.size()));
}

QVariantList ChatSession::disappearingOptions() const {
    QVariantList out;
    for (const auto& option : data::Disappearing::all()) {
        out.append(QVariantMap{{"seconds", option.seconds.value_or(0)},
                               {"label", QString::fromUtf8(option.label.data(), static_cast<qsizetype>(option.label.size()))}});
    }
    return out;
}

void ChatSession::setDisappearing(int seconds) {
    if (currentChatId_.empty()) return;
    const auto timer = data::Disappearing::fromSeconds(seconds > 0 ? std::optional<int>(seconds) : std::nullopt);
    auto repository = repository_;
    const std::string chatId = currentChatId_;
    runInBackground(
        pool_, this, [repository, chatId, timer]() { repository->setDisappearing(chatId, timer); },
        [this, chatId, timer]() {
            if (auto* chat = chatList_.find(chatId)) chat->disappearingSeconds = timer.seconds;
            emit currentChatChanged();
        },
        [this](const Failure& failure) { fail(failure); });
}

QVariantList ChatSession::forwardTargets() const {
    QVariantList out;
    for (const auto& chat : chatList_.chats()) {
        const auto& meta = *chat.meta;
        if (meta.community && !meta.community->canPost) continue;  // a channel this member only reads
        const QString title = QString::fromStdString(meta.handle);
        out.append(QVariantMap{
            {"chatId", QString::fromStdString(meta.id)},
            {"title", title},
            {"initial", title.isEmpty() ? QStringLiteral("?") : title.left(1).toUpper()},
            {"isGroup", meta.isGroup},
            {"avatarUrl", meta.isGroup ? QString() : QString::fromStdString(meta.avatarUrl.value_or(""))},
            {"communityImage", communityAvatarPath(meta)},
            {"userId", meta.isGroup ? QString() : QString::fromStdString(meta.otherUserId)},
        });
    }
    return out;
}

void ChatSession::forwardMessages(const QStringList& messageIds, const QStringList& chatIds) {
    struct Item {
        std::string text;
        std::optional<model::FileDescriptor> file;
        std::int64_t at = 0;
        /// A post forwarded from a channel stays one, so every copy keeps counting toward its views.
        std::optional<model::ChannelPostRef> channelPost;
    };
    std::vector<Item> items;
    for (const auto& id : messageIds) {
        const auto* message = messages_.find(id.toStdString());
        if (!message || message->kind == data::MessageKind::Undecryptable) continue;
        if (message->kind == data::MessageKind::File && !message->file) continue;
        items.push_back(Item{message->text, message->file, message->createdAtMs, message->channelPost});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.at < b.at; });
    std::vector<std::shared_ptr<const data::ConversationMeta>> targets;
    for (const auto& chatId : chatIds) {
        if (auto meta = metaFor(chatId.toStdString())) targets.push_back(std::move(meta));
    }
    if (items.empty() || targets.empty()) {
        emit forwardDone(0, {});
        return;
    }
    // Before anything goes out, so a chat that can't take it doesn't leave the rest half done.
    for (const auto& meta : targets) {
        if (const QString why = sendRefusal(meta->id, device_key_refusal::kForward); !why.isEmpty()) {
            emit forwardDone(0, why);
            return;
        }
    }
    auto repository = repository_;
    auto files = files_;
    runInBackground(
        pool_, this,
        [repository, files, items, targets]() {
            int sent = 0;
            for (const auto& item : items) {
                if (item.file) {
                    // A file belongs to the chat it was uploaded to: decrypt it here and send a fresh copy.
                    const auto plain = files->download(*item.file);
                    for (const auto& meta : targets) {
                        files->send(*meta, plain, item.file->filename, item.file->mimeType);
                        ++sent;
                    }
                    std::error_code ignored;
                    std::filesystem::remove_all(plain.parent_path(), ignored);
                } else {
                    for (const auto& meta : targets) {
                        if (item.channelPost) {
                            repository->sendChannelForward(*meta, item.text, *item.channelPost);
                        } else {
                            repository->sendForwarded(*meta, item.text);
                        }
                        ++sent;
                    }
                }
            }
            return sent;
        },
        [this](int sent) {
            emit forwardDone(sent, {});
            refreshAll();
        },
        [this](const Failure& failure) {
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            emit forwardDone(0, failure.message);
            refreshAll();
        });
}

void ChatSession::forwardChannelPost(const QString& text, const QString& channelId, const QString& postId, const QString& channelName,
                                     const QStringList& chatIds) {
    std::vector<std::shared_ptr<const data::ConversationMeta>> targets;
    for (const auto& chatId : chatIds) {
        if (auto meta = metaFor(chatId.toStdString())) targets.push_back(std::move(meta));
    }
    if (text.trimmed().isEmpty() || targets.empty()) {
        emit forwardDone(0, {});
        return;
    }
    for (const auto& meta : targets) {
        if (const QString why = sendRefusal(meta->id, device_key_refusal::kForward); !why.isEmpty()) {
            emit forwardDone(0, why);
            return;
        }
    }
    auto repository = repository_;
    const std::string body = text.toStdString();
    const model::ChannelPostRef ref{channelId.toStdString(), postId.toStdString(), channelName.toStdString()};
    runInBackground(
        pool_, this,
        [repository, body, ref, targets]() {
            int sent = 0;
            for (const auto& meta : targets) {
                repository->sendChannelForward(*meta, body, ref);
                ++sent;
            }
            return sent;
        },
        [this](int sent) {
            emit forwardDone(sent, {});
            refreshAll();
        },
        [this](const Failure& failure) {
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            emit forwardDone(0, failure.code.isEmpty() ? QStringLiteral("Couldn't forward it. Try again.") : failure.message);
            refreshAll();
        });
}

bool ChatSession::canEdit(const QString& messageId) const {
    constexpr std::int64_t kEditWindowMs = 24LL * 60 * 60 * 1000;
    const auto* message = messages_.find(messageId.toStdString());
    return message && message->fromMe && message->kind == data::MessageKind::Text &&
           util::nowMillis() - message->createdAtMs < kEditWindowMs;
}

QString ChatSession::lastEditableId() const {
    const auto& all = messages_.all();
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        if (!it->fromMe) continue;
        const QString id = QString::fromStdString(it->id);
        return canEdit(id) ? id : QString();
    }
    return {};
}

void ChatSession::editMessage(const QString& messageId, const QString& text) {
    const auto meta = currentMetaShared();
    const auto* message = messages_.find(messageId.toStdString());
    if (!meta || !message || sending_) return;
    if (!canEdit(messageId)) {
        setError(QStringLiteral("Only your own messages from the last 24 hours can be edited."));
        emit edited(false);
        return;
    }
    const std::string body = text.trimmed().toStdString();
    if (body.empty() || body == message->text) {
        emit edited(body == message->text);
        return;
    }
    if (const QString why = sendRefusal(meta->id, device_key_refusal::kChange); !why.isEmpty()) {
        setError(why);
        emit edited(false);
        return;
    }
    sending_ = true;
    emit sendingChanged();
    auto repository = repository_;
    const std::string id = message->id;
    const std::string chatId = currentChatId_;
    runInBackground(
        pool_, this, [repository, meta, id, body]() { repository->sendEdit(*meta, id, body); },
        [this, chatId, id, body]() {
            sending_ = false;
            emit sendingChanged();
            emit edited(true);
            // Show the new text right away; the next poll brings the server's view.
            if (auto* chat = chatList_.find(chatId)) {
                for (auto& m : chat->messages) {
                    if (m.id != id) continue;
                    m.earlierVersions.push_back(data::EditVersion{m.text, m.editedAtMs.value_or(m.createdAtMs)});
                    m.text = body;
                    m.editedAtMs = util::nowMillis();
                }
                if (chatId == currentChatId_) showThread(chat->meta, chat->messages);
                chatList_.setChats(chatList_.snapshot());
            }
            refreshThread();
        },
        [this](const Failure& failure) {
            sending_ = false;
            emit sendingChanged();
            emit edited(false);
            fail(failure);
        });
}

QVariantList ChatSession::editHistory(const QString& messageId) const {
    QVariantList out;
    const auto* message = messages_.find(messageId.toStdString());
    if (!message) return out;
    for (const auto& version : message->earlierVersions) {
        out.append(QVariantMap{{"text", QString::fromStdString(version.text)},
                               {"time", formatDay(version.atMs) + QStringLiteral(", ") + formatClock(version.atMs)}});
    }
    const std::int64_t at = message->editedAtMs.value_or(message->createdAtMs);
    out.append(QVariantMap{{"text", QString::fromStdString(message->text)},
                           {"time", formatDay(at) + QStringLiteral(", ") + formatClock(at)},
                           {"current", true}});
    return out;
}

void ChatSession::deleteForMe(const QString& messageId) {
    if (!messages_.find(messageId.toStdString())) return;
    prefs::hideMessage(identity_->userId, messageId);
    removeLocally(messageId.toStdString());
}

void ChatSession::deleteForEveryone(const QString& messageId) {
    const auto* message = messages_.find(messageId.toStdString());
    if (!message || !message->fromMe) return;
    auto repository = repository_;
    const std::string id = message->id;
    const auto related = deletedWith(*message, messages_.all());
    runInBackground(
        pool_, this,
        [repository, id, related]() {
            repository->deleteMessage(id);
            for (const auto& other : related) repository->deleteMessage(other);
        },
        [this, id, related]() {
            removeLocally(id);
            for (const auto& other : related) removeLocally(other);
            refreshThread();
        },
        [this](const Failure& failure) { fail(failure); });
}

void ChatSession::deleteManyForMe(const QStringList& messageIds) {
    for (const auto& id : messageIds) deleteForMe(id);
}

void ChatSession::deleteManyForEveryone(const QStringList& messageIds) {
    std::vector<std::string> ids;
    for (const auto& id : messageIds) {
        const auto* message = messages_.find(id.toStdString());
        if (!message || !message->fromMe) continue;
        ids.push_back(message->id);
        const auto related = deletedWith(*message, messages_.all());
        ids.insert(ids.end(), related.begin(), related.end());
    }
    if (ids.empty()) return;
    // A selected pin notice may also be one of another selected message's.
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    auto repository = repository_;
    runInBackground(
        pool_, this,
        [repository, ids]() {
            for (const auto& id : ids) repository->deleteMessage(id);
        },
        [this, ids]() {
            for (const auto& id : ids) removeLocally(id);
            refreshThread();
        },
        [this](const Failure& failure) {
            fail(failure);
            refreshThread();
        });
}

bool ChatSession::allFromMe(const QStringList& messageIds) const {
    if (messageIds.isEmpty()) return false;
    for (const auto& id : messageIds) {
        const auto* message = messages_.find(id.toStdString());
        if (!message || !message->fromMe || message->kind == data::MessageKind::Undecryptable) return false;
    }
    return true;
}

bool ChatSession::anyFile(const QStringList& messageIds) const {
    for (const auto& id : messageIds) {
        const auto* message = messages_.find(id.toStdString());
        if (message && message->kind == data::MessageKind::File) return true;
    }
    return false;
}

QStringList ChatSession::forwardable(const QStringList& messageIds) const {
    QStringList out;
    for (const auto& id : messageIds) {
        const auto* message = messages_.find(id.toStdString());
        if (message && message->kind != data::MessageKind::Undecryptable) out.append(id);
    }
    return out;
}

void ChatSession::removeLocally(const std::string& messageId) {
    if (auto* chat = chatList_.find(currentChatId_)) {
        std::erase_if(chat->messages, [&](const data::Message& m) { return m.id == messageId; });
        chatList_.setChats(chatList_.snapshot());
    }
    messages_.remove(messageId);
}

QString ChatSession::textOf(const QString& messageId) const {
    const auto* message = messages_.find(messageId.toStdString());
    return message ? QString::fromStdString(message->text) : QString();
}

QString ChatSession::previewOf(const QString& messageId) const {
    const auto* message = messages_.find(messageId.toStdString());
    if (!message) return {};
    const std::string text = message->kind == data::MessageKind::File ? kPaperclip + message->text : message->text;
    return QString::fromStdString(model::rich_message::makePreview(text));
}

// --- Files ---

void ChatSession::attachFile(const QUrl& file) {
    const auto meta = currentMetaShared();
    if (!meta || !fileActivity_.isEmpty()) return;
    if (refuseSend(meta->id, device_key_refusal::kMessage)) return;
    const fs::path path = pathFromUrl(file);
    const std::string chatId = currentChatId_;
    auto files = files_;
    setFileActivity(QStringLiteral("Encrypting…"));
    // Stage updates arrive on the worker thread; hop to the UI thread to show them.
    QPointer<ChatSession> self(this);
    auto onStage = [self](data::FileSendStage stage) {
        QMetaObject::invokeMethod(
            qApp,
            [self, stage] {
                if (!self || self->fileActivity_.isEmpty()) return;
                self->setFileActivity(stage == data::FileSendStage::Encrypting  ? QStringLiteral("Encrypting…")
                                      : stage == data::FileSendStage::Uploading ? QStringLiteral("Uploading…")
                                                                                : QStringLiteral("Sending…"));
            },
            Qt::QueuedConnection);
    };
    // A pasted picture's temporary copy goes once it's sent (or failed to).
    const bool pastedCopy = path.parent_path() == cacheDir_ / "pasted";
    if (outbox_) {
        // Tor routing: a sealed copy goes into the outbox (the original may
        // change or move), then it's sent over Tor when Tor is connected.
        setFileActivity(QStringLiteral("Queuing…"));
        auto outbox = outbox_;
        const std::string conversationId = meta->id;
        runInBackground(
            pool_, this,
            [outbox, conversationId, path, pastedCopy]() {
                struct RemoveAfter {
                    fs::path path;
                    bool enabled;
                    ~RemoveAfter() {
                        std::error_code ignored;
                        if (enabled) fs::remove(path, ignored);
                    }
                } cleanup{path, pastedCopy};
                const auto u8 = path.filename().u8string();
                outbox->addFile(conversationId, path, std::string(u8.begin(), u8.end()),
                                data::FileRepository::guessMimeType(path));
            },
            [this]() {
                setFileActivity({});
                publishQueue();
                drainOutbox();
            },
            [this](const Failure& failure) {
                setFileActivity({});
                fail(failure);
            });
        return;
    }
    runInBackground(
        pool_, this,
        [files, meta, path, onStage, pastedCopy]() {
            struct RemoveAfter {
                fs::path path;
                bool enabled;
                ~RemoveAfter() {
                    std::error_code ignored;
                    if (enabled) fs::remove(path, ignored);
                }
            } cleanup{path, pastedCopy};
            return files->send(*meta, path, onStage);
        },
        [this, chatId](data::Message message) {
            setFileActivity({});
            if (message.security) securityById_[message.id] = *message.security;
            if (auto* chat = chatList_.find(chatId)) {
                chat->messages.push_back(message);
                if (chatId == currentChatId_) showThread(chat->meta, chat->messages);
                chatList_.setChats(chatList_.snapshot());
            }
            refreshThread();
        },
        [this](const Failure& failure) {
            setFileActivity({});
            fail(failure);
        });
}

QStringList ChatSession::pastedAttachments() {
    const QMimeData* data = QGuiApplication::clipboard()->mimeData();
    if (data == nullptr) return {};
    QStringList out;
    if (data->hasUrls()) {
        for (const QUrl& url : data->urls()) {
            if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) out.append(url.toString());
        }
        if (!out.isEmpty()) return out;
    }
    if (data->hasImage()) {
        const QImage image = qvariant_cast<QImage>(data->imageData());
        if (image.isNull()) return {};
        // A screenshot has no file: write one (PNG) where the session's files go, deleted after sending.
        const fs::path folder = cacheDir_ / "pasted";
        std::error_code ignored;
        util::makePrivateDir(folder, ignored);
        const QString name = QStringLiteral("Pasted image %1.png").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss")));
        const QString path = QString::fromStdWString((folder / name.toStdWString()).wstring());
        util::makePrivateFile(folder / name.toStdWString());
        if (image.save(path, "PNG")) out.append(QUrl::fromLocalFile(path).toString());
    }
    return out;
}

void ChatSession::discardPasted(const QString& fileUrl) {
    const fs::path path = pathFromUrl(QUrl(fileUrl));
    if (path.parent_path() != cacheDir_ / "pasted") return;
    std::error_code ignored;
    fs::remove(path, ignored);
}

QVariantMap ChatSession::localFileInfo(const QString& fileUrl) const {
    const QFileInfo info(QUrl(fileUrl).toLocalFile());
    const QString suffix = info.suffix().toLower();
    const bool image = QStringList{"jpg", "jpeg", "png", "gif", "webp", "bmp"}.contains(suffix);
    return {{"name", info.fileName()},
            {"size", QString::fromStdString(model::file_message::formatBytes(info.size()))},
            {"image", image},
            {"exists", info.isFile()}};
}

bool ChatSession::canOpenFile(const QString& messageId) const {
    const auto* message = messages_.find(messageId.toStdString());
    return message && message->file && file_safety::isSafeToOpen(QString::fromStdString(message->file->filename));
}

void ChatSession::openFile(const QString& messageId) {
    const auto* message = messages_.find(messageId.toStdString());
    if (!message || !message->file || !fileActivity_.isEmpty()) return;
    if (!canOpenFile(messageId)) {
        setError(QStringLiteral("For your safety, only pictures, audio, video, text and PDF files open from a chat. Save this one instead."));
        return;
    }
    auto files = files_;
    const auto descriptor = *message->file;
    setFileActivity(QStringLiteral("Decrypting ") + QString::fromStdString(descriptor.filename) + QStringLiteral("…"));
    runInBackground(
        pool_, this,
        [files, descriptor]() {
            const auto path = files->download(descriptor);
            file_safety::markFromInternet(path);
            return path;
        },
        [this](fs::path path) {
            setFileActivity({});
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(path.wstring())))) {
                setError(QStringLiteral("There's no app on this computer to open that kind of file. Save it instead."));
            }
        },
        [this](const Failure& failure) {
            setFileActivity({});
            fail(failure);
        });
}

void ChatSession::saveFile(const QString& messageId, const QUrl& destination) {
    const auto* message = messages_.find(messageId.toStdString());
    if (!message || !message->file || !fileActivity_.isEmpty()) return;
    auto files = files_;
    const auto descriptor = *message->file;
    const fs::path target = pathFromUrl(destination);
    setFileActivity(QStringLiteral("Decrypting ") + QString::fromStdString(descriptor.filename) + QStringLiteral("…"));
    runInBackground(
        pool_, this,
        [files, descriptor, target]() {
            const auto decrypted = files->download(descriptor);
            // The save dialog has already asked before replacing an existing file.
            fs::copy_file(decrypted, target, fs::copy_options::overwrite_existing);
            file_safety::markFromInternet(target);
            // The decrypted copy in the cache isn't needed once it's saved.
            std::error_code ignored;
            fs::remove_all(decrypted.parent_path(), ignored);
            return target;
        },
        [this](fs::path saved) {
            setFileActivity({});
            emit fileSaved(displayPath(saved));
        },
        [this](const Failure& failure) {
            setFileActivity({});
            fail(failure);
        });
}

// --- Groups ---

template <typename Work>
void ChatSession::groupAction(Work work, const std::string& openAfter) {
    if (groupBusy_) return;
    setGroupBusy(true);
    runInBackground(
        pool_, this, std::move(work),
        [this, openAfter](auto&&... createdId) {
            setGroupBusy(false);
            if constexpr (sizeof...(createdId) == 1) {
                pendingOpenChatId_ = (createdId, ...);
            } else if (!openAfter.empty()) {
                pendingOpenChatId_ = openAfter;
            }
            // Members, roles or the name changed: re-resolve now rather than within the minute.
            if (!currentChatId_.empty()) staleMetas_.insert(currentChatId_);
            emit groupActionDone({});
            refreshAll();
        },
        [this](const Failure& failure) {
            setGroupBusy(false);
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            emit groupActionDone(failure.message);
        });
}

void ChatSession::createGroup(const QString& name, const QStringList& memberIds) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || memberIds.isEmpty()) {
        emit groupActionDone(QStringLiteral("Give the group a name and pick at least one friend."));
        return;
    }
    auto groups = groups_;
    auto identity = identity_;
    std::vector<std::string> ids;
    for (const auto& id : memberIds) ids.push_back(id.toStdString());
    const std::string title = trimmed.toStdString();
    groupAction([groups, identity, ids, title]() {
        // Public keys come fresh from the server, not from anything the UI held.
        std::vector<api::FriendDto> members;
        for (const auto& friendDto : groups->friends()) {
            if (std::find(ids.begin(), ids.end(), friendDto.userId) != ids.end()) members.push_back(friendDto);
        }
        if (members.empty()) throw api::UserFacingError("Those people aren't your friends any more.");
        return groups->create(*identity, title, members);
    });
}

void ChatSession::startMessageRequest(const QString& userIdText) {
    if (const QString existing = chatIdForUser(userIdText); !existing.isEmpty()) {
        openChat(existing);
        emit groupActionDone({});
        return;
    }
    auto api = api_;
    auto identity = identity_;
    auto keys = keys_;
    const std::string userId = userIdText.toStdString();
    groupAction([api, identity, keys, userId]() {
        // Their key comes fresh from the server; the same sealing as accepting a friend request.
        const auto user = api->getUser(userId);
        if (user.deleted) throw api::UserFacingError("That account was deleted.");
        keys->requireUnchangedKey(userId, user.publicKey);
        const auto key = crypto::generateConversationKey();
        std::vector<api::WrappedKeyForUser> participants;
        participants.push_back({identity->userId, crypto::base64Encode(crypto::sealConversationKey(key, identity->publicKey))});
        participants.push_back({userId, crypto::base64Encode(crypto::sealConversationKey(key, crypto::base64Decode(user.publicKey)))});
        api::CreateConversationRequest request;
        request.isGroup = false;
        request.participants = std::move(participants);
        return api->createConversation(request).id;
    });
}

void ChatSession::addMembers(const QStringList& userIds) {
    const auto meta = currentMetaShared();
    if (!meta || !meta->isGroup || userIds.isEmpty()) return;
    auto groups = groups_;
    std::vector<std::string> ids;
    for (const auto& id : userIds) ids.push_back(id.toStdString());
    groupAction([groups, meta, ids]() {
        for (const auto& friendDto : groups->friends()) {
            if (std::find(ids.begin(), ids.end(), friendDto.userId) != ids.end()) groups->add(*meta, friendDto);
        }
    });
}

void ChatSession::removeMember(const QString& userId) {
    const auto meta = currentMetaShared();
    if (!meta || !meta->isGroup) return;
    auto groups = groups_;
    const std::string id = userId.toStdString();
    const std::string self = identity_->userId;
    groupAction([groups, meta, id, self]() { groups->remove(meta->id, id, self); });
}

void ChatSession::setMemberRole(const QString& userId, const QString& role) {
    const auto meta = currentMetaShared();
    if (!meta || !meta->isGroup) return;
    auto groups = groups_;
    const std::string id = userId.toStdString();
    const std::string newRole = role.toStdString();
    groupAction([groups, meta, id, newRole]() { groups->setRole(meta->id, id, newRole); });
}

void ChatSession::renameGroup(const QString& name) {
    const auto meta = currentMetaShared();
    const QString trimmed = name.trimmed();
    if (!meta || !meta->isGroup || trimmed.isEmpty()) return;
    auto groups = groups_;
    const std::string title = trimmed.toStdString();
    groupAction([groups, meta, title]() { groups->rename(*meta, title); });
}

void ChatSession::leaveGroup() {
    const auto meta = currentMetaShared();
    if (!meta || !meta->isGroup) return;
    auto groups = groups_;
    const std::string self = identity_->userId;
    groupAction([groups, meta, self]() { groups->remove(meta->id, self, self); });
}

// --- Helpers ---

void ChatSession::noticeIncoming(const ChatSummary& chat, std::int64_t historyBefore) {
    int fresh = 0;
    for (const auto& message : chat.messages) {
        if (message.fromMe) continue;
        if (knownIncoming_.insert(message.id).second && message.createdAtMs >= historyBefore) ++fresh;
    }
    // Muted contacts never notify.
    const bool muted = !chat.meta->isGroup && prefs::muted(identity_->userId).contains(QString::fromStdString(chat.meta->otherUserId));
    if (fresh > 0 && incomingBaseline_ && !muted) {
        emit incomingMessages(QString::fromStdString(chat.meta->id), QString::fromStdString(chat.meta->handle), fresh,
                              chat.meta->id == currentChatId_);
    }
}

std::vector<data::Message> ChatSession::withKnownSecurity(std::vector<data::Message> messages) const {
    for (auto& message : messages) {
        if (const auto it = securityById_.find(message.id); it != securityById_.end()) message.security = it->second;
    }
    return messages;
}

// --- The Tor outbox ---

int ChatSession::heldCount() const {
    return static_cast<int>(std::count_if(queued_.begin(), queued_.end(), [](const data::OutboxItem& i) { return i.held; }));
}

void ChatSession::showThread(std::shared_ptr<const data::ConversationMeta> meta, std::vector<data::Message> messages) {
    if (outbox_ && meta) {
        for (const auto& item : queued_) {
            if (item.conversationId != meta->id) continue;
            data::Message m;
            m.id = item.id;
            m.fromMe = true;
            m.senderId = identity_->userId;
            m.kind = data::MessageKind::Text;
            m.text = item.kind == data::OutboxItem::Kind::File ? kPaperclip + item.text : item.text;
            m.createdAtMs = item.createdAtMs;
            m.queued = true;
            m.queueError = item.lastError;
            m.queueHeld = item.held;
            if (item.replyToId) m.replyTo = data::ReplyView{*item.replyToId, item.replyPreview, item.replyFromMe};
            messages.push_back(std::move(m));
        }
    }
    messages_.setMessages(std::move(meta), std::move(messages));
}

void ChatSession::publishQueue() {
    if (!outbox_) return;
    queued_ = outbox_->items();
    int files = 0;
    for (const auto& item : queued_) files += item.kind == data::OutboxItem::Kind::File ? 1 : 0;
    if (tor_) tor_->setQueued(static_cast<int>(queued_.size()), files);
    emit queuedChanged();
    if (const auto* chat = chatList_.find(currentChatId_)) showThread(chat->meta, withKnownSecurity(chat->messages));
}

void ChatSession::discardQueued(const QString& id) {
    if (!outbox_) return;
    outbox_->remove(id.toStdString());
    publishQueue();
}

void ChatSession::retryQueued(const QString& id) {
    if (!outbox_) return;
    outbox_->release(id.toStdString());
    publishQueue();
    drainOutbox();
}

namespace {

/// What one pass over the outbox did.
struct DrainResult {
    std::vector<std::pair<std::string, data::Message>> sent;  // chat id, message as sent
};

/// Worth trying again later (no connection, Tor down, server busy) rather than holding for the person.
bool transient(const api::ApiError& e) {
    return e.status() == 0 || e.status() >= 500 || e.status() == 429 || e.status() == 401 ||
           (e.status() == 409 && e.code() == "upload_incomplete");
}

}  // namespace

void ChatSession::drainOutbox() {
    if (!outbox_ || draining_ || tor_ == nullptr || !tor_->connected()) return;
    std::vector<data::OutboxItem> items;
    std::map<std::string, std::shared_ptr<const data::ConversationMeta>> metas;
    for (const auto& item : outbox_->items()) {
        if (item.held) continue;
        // A chat not loaded yet (first poll still out) waits for the next round.
        const auto it = metas_.find(item.conversationId);
        if (it == metas_.end()) continue;
        // Queued before the chat moved to device keys or its key was refused: it waits, as any refused send does.
        if (const QString why = sendRefusal(item.conversationId, device_key_refusal::kMessage); !why.isEmpty()) {
            outbox_->recordAttempt(item.id, why.toStdString(), /*hold=*/true);
            publishQueue();
            continue;
        }
        metas[item.conversationId] = it->second;
        items.push_back(item);
    }
    if (items.empty()) return;
    draining_ = true;
    auto outbox = outbox_;
    auto repository = repository_;
    auto files = files_;
    const fs::path staging = cacheDir_ / "outbox-send";
    runInBackground(
        pool_, this,
        [outbox, repository, files, staging, items, metas]() {
            DrainResult result;
            for (const auto& item : items) {
                const auto& meta = *metas.at(item.conversationId);
                try {
                    data::Message message;
                    if (item.kind == data::OutboxItem::Kind::Text) {
                        std::optional<data::ReplyTarget> reply;
                        if (item.replyToId) {
                            reply = data::ReplyTarget{{*item.replyToId, item.replyToSender, item.replyPreview}, item.replyFromMe};
                        }
                        // The item's id is the clientMessageId: a retry never stores it twice.
                        message = repository->send(meta, item.text, reply, item.id);
                    } else {
                        util::makePrivateDir(staging);
                        struct RemoveAfter {
                            fs::path path;
                            ~RemoveAfter() {
                                std::error_code ignored;
                                fs::remove(path, ignored);
                            }
                        } plain{staging / item.id};
                        outbox->openFile(item, plain.path);
                        message = files->send(meta, plain.path, item.text, item.mimeType, {}, item.id);
                    }
                    outbox->remove(item.id);
                    result.sent.emplace_back(item.conversationId, std::move(message));
                } catch (const api::ApiError& e) {
                    if (transient(e)) {
                        outbox->recordAttempt(item.id, e.what());
                        break;  // Tor or the server is down: the rest wait too, in order
                    }
                    outbox->recordAttempt(item.id, e.what(), /*hold=*/true);
                } catch (const api::TransferInterrupted& e) {
                    outbox->recordAttempt(item.id, e.what());
                    break;
                } catch (const std::exception& e) {
                    outbox->recordAttempt(item.id, e.what(), /*hold=*/true);
                }
            }
            return result;
        },
        [this](DrainResult result) {
            draining_ = false;
            for (auto& [chatId, message] : result.sent) {
                if (message.security) securityById_[message.id] = *message.security;
                if (auto* chat = chatList_.find(chatId)) chat->messages.push_back(std::move(message));
            }
            publishQueue();
            if (!result.sent.empty()) {
                chatList_.setChats(chatList_.snapshot());
                refreshThread();
            }
        },
        [this](const Failure&) {
            draining_ = false;
            publishQueue();
        });
}

void ChatSession::fail(const Failure& failure) {
    if (failure.status == 401) {
        emit sessionExpired();
        return;
    }
    setError(failure.message);
}

void ChatSession::setError(const QString& error) {
    if (error_ == error) return;
    error_ = error;
    emit errorChanged();
}

void ChatSession::setFileActivity(const QString& activity) {
    if (fileActivity_ == activity) return;
    fileActivity_ = activity;
    emit fileActivityChanged();
}

void ChatSession::setGroupBusy(bool busy) {
    if (groupBusy_ == busy) return;
    groupBusy_ = busy;
    emit groupBusyChanged();
}

}  // namespace redvault::app
